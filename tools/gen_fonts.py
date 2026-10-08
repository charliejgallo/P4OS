#!/usr/bin/env python3
"""Generate the P4OS font set (from AmoledOS) (components/aos_fonts/).

Why this exists
---------------
The Montserrat fonts that ship with LVGL are built with

    -r 0x20-0x7F,0xB0,0x2022

which is pure ASCII: they contain no accented vowels, no n-tilde and no
inverted question mark. Any Spanish text written properly renders with holes,
because LVGL draws nothing at all when a glyph is missing - no error, no log,
no replacement box. `aos_app_settings.c` has carried one such hole ("Toca el
centro de la cruz", with an accent in the source) since it was written.

This script regenerates the same six sizes the firmware uses, with the Latin-1
supplement added, so accented text renders. It is the prerequisite for the
multi-language work: every translation pack past English needs glyphs the
stock fonts do not have.

The generated fonts live alongside LVGL's own rather than replacing them.
That is deliberate: turning off CONFIG_LV_FONT_MONTSERRAT_* would drop those
symbols from the dynamic-app symbol table, and the four apps that reference
lv_font_montserrat_* directly (flappy, arkanos, g2043, hello_app) would stop
loading. The duplicate costs ~116 KB of flash against 2.89 MB free; clean it
up once those apps are migrated to the aos_font_* pointers.

Requirements
------------
    brew install node
    npm install -g lv_font_conv

Usage
-----
    python3 tools/gen_fonts.py            # regenerate everything
    python3 tools/gen_fonts.py --check    # report what would change, write nothing

Regenerate after changing TEXT_RANGE or SIZES, then rebuild the firmware and
run tools/gen_symbols.py so the dynamic apps can see the new fonts.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(REPO, "sim", "lvgl", "scripts", "built_in_font")
OUT_DIR = os.path.join(REPO, "components", "aos_fonts")
INC_DIR = os.path.join(OUT_DIR, "include")

# Sizes the UI actually asks for. aos_theme.c maps them to the four roles:
# huge=48 (watch faces), title=28, body=20, small=16. 14 is the LVGL default
# and is used directly by a few widgets.
# P4OS: the 5" panel needs the bigger steps too (status bar 24, titles 32,
# the big clock and the lock screen 64).
SIZES = [14, 16, 20, 24, 28, 32, 36, 48, 64]

# Unicode ranges baked into every size:
#
#   0x20-0x7F   ASCII
#   0xA0-0xFF   Latin-1 supplement. The accented vowels, n-tilde, the inverted
#               question and exclamation marks, the degree sign. Covers
#               Spanish, Portuguese, French, German, Italian and Catalan.
#   0x2022      bullet - LVGL widgets use it for password fields
#   0x20AC      euro sign, which is NOT part of Latin-1
#
# Cost over plain ASCII across all six sizes, read off the linker map:
# 237.1 KB for 158 glyphs becomes 352.3 KB for 254, so +115 KB of flash.
#
# Adding Latin Extended-A (0x100-0x17F: Polish, Czech, Slovak, Turkish,
# Croatian, Hungarian, Romanian, Lithuanian, Slovenian) would push the bitmap
# arrays from +102 KB to +254 KB over ASCII - call it +280 KB compiled, well
# over twice this range. Montserrat covers that block completely, so enabling
# it is this one string plus a rebuild, but it is not the free lunch the glyph
# count suggests: those 128 glyphs are as wide as the letters they decorate.
TEXT_RANGE = ("0x20-0x7F,0xA0-0xFF,0x2022,0x2026,0x20AC,"
              # P4OS, for the bench: Delta Omega mu pi, arrows, minus, sqrt,
              # infinity, approx, not-equal, less/greater-or-equal
              "0x394,0x3A9,0x3BC,0x3C0,0x2190-0x2193,0x2212,0x221A,0x221E,0x2248,0x2260,0x2264,0x2265")

# P4OS, 2026-10-07: what arrives from outside carries more than Latin-1. A
# Mac calls itself "Charlie's MacBook" with a typographic apostrophe (U+2019),
# BLE devices and notifications bring dashes, quotes, units, arrows, other
# alphabets. Inter has them, and lv_font_conv takes only the code points the
# font really has, so a block can be named whole. Measured per block, summed
# over the text sizes (14-36 and the labels' 20): the symbols below ~410 KB,
# Latin Extended-A ~150 KB, Greek ~110 KB; Cyrillic would be ~300 KB more and
# is left out. The 48 and 64, where a glyph costs ~0.6 and ~1 KB, get only
# the typographic punctuation (EXTRA_BIG): they are clocks and titles.
EXTRA_PUNCT = "0x2010-0x205E"               # dashes, quotes, daggers, per mille, primes, angle quotes
EXTRA_TEXT = ",".join([
    EXTRA_PUNCT,
    "0x100-0x17F",                          # Latin Extended-A: Polish, Czech, Turkish, Romanian...
    "0x218-0x21B",                          # Romanian's comma-below S and T
    "0x370-0x3FF",                          # Greek (and the engineer's alpha to omega)
    "0x2070-0x209F",                        # superscripts and subscripts
    "0x20A0-0x20C0",                        # currencies
    "0x2100-0x218F",                        # letterlike (TM, No, degree C) and fractions
    "0x2194-0x21FF",                        # the rest of the arrows
    "0x2200-0x22FF",                        # mathematical operators
    "0x2300-0x23FF",                        # technical (command, option, return...)
    "0x25A0-0x25FF",                        # geometric shapes
    "0x2600-0x27BF",                        # miscellaneous symbols and dingbats (check marks, stars)
])
EXTRA_BIG = EXTRA_PUNCT
BIG_SIZES = (48, 64)


def text_range(size):
    return TEXT_RANGE + "," + (EXTRA_BIG if size in BIG_SIZES else EXTRA_TEXT)

# The FontAwesome codepoints behind every LV_SYMBOL_* macro, copied verbatim
# from LVGL's own scripts/built_in_font/built_in_font_gen.py - if that part of
# this list drifts from LVGL's, the symbols in the status bar and the launcher
# vanish - PLUS 71 more that exist for the emoji.
#
# The watch cannot draw emoji: they are thousands of glyphs and they are in
# colour. But the phone sends them constantly, and until now every one of them
# came out as a bullet (see aos_text_safe.c). FontAwesome, which this project
# already ships, turns out to carry a face set that maps one-to-one onto the
# most used ones - grin-tears IS the crying-laughing face, sad-cry IS the
# sobbing one - plus the heart, the fire, the thumb, the star. So the emoji get
# drawn with glyphs that were already paid for, in the same filled style as the
# rest of the watch's symbols, and with no new font in the tree.
#
# The mapping lives in aos_text_safe.c. Anything without an analogue - the
# hundred-points, the sparkles - keeps falling back to the bullet.
SYMBOL_RANGE = (
    "61441,61444,61445,61448,61451,61452,61453,61457,61459,61461,61463,"
    "61465,61468,61473,61478,61479,61480,61488,61502,61507,61512,61515,"
    "61516,61517,61521,61522,61523,61524,61543,61544,61547,61549,61550,"
    "61552,61553,61554,61556,61559,61560,61561,61562,61563,61585,61587,"
    "61589,61604,61606,61634,61636,61637,61639,61641,61664,61671,61673,"
    "61674,61683,61684,61692,61720,61721,61722,61724,61732,61749,61787,"
    "61796,61797,61829,61830,61881,61883,61922,61923,61931,61949,62006,"
    "62016,62017,62018,62019,62020,62043,62087,62099,62172,62183,62189,"
    "62206,62212,62373,62405,62539,62658,62680,62682,62796,62806,62810,"
    "62823,62841,62842,62847,62848,62850,62851,62852,62854,62855,62856,"
    "62857,62859,62860,62870,62875,62884,62885,62899,62900,62904,62914,"
    "62920,63108,63166,63187,63202,63244,63391,63401,63426,63650"
)

# P4OS: Inter (SIL OFL 1.1), chosen on 2026-09-27 against eleven others
# (bench/results/fonts): Medium for text, SemiBold for the home icons' names.
INTER_CACHE = os.path.expanduser("~/.cache/p4os-fonts")
# The full family from the upstream release (the web subsets have no Greek or
# maths): extras/ttf/ of Inter-4.1.zip, unzipped into the cache once.
INTER_ZIP = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
TEXT_FONT = os.path.join(INTER_CACHE, "interzip", "extras", "ttf", "Inter-Medium.ttf")
LABEL_FONT = os.path.join(INTER_CACHE, "interzip", "extras", "ttf", "Inter-SemiBold.ttf")
LABEL_SIZES = [20]
# Digits only, for clocks, timers and big readings: aos_inter_num_<px>.
NUM_SIZES = [96, 144]
NUM_RANGE = "0x20,0x25,0x2B-0x3A,0x68,0x6D,0x73,0xB0"   # space % + , - . / 0-9 : h m s °
SYMBOL_FONT = "FontAwesome5-Solid+Brands+Regular.woff"
BPP = 4

PREFIX = "aos_inter_"
LABEL_PREFIX = "aos_inter_sb_"


def die(msg):
    sys.exit("gen_fonts: " + msg)


def check_tools():
    if not shutil.which("lv_font_conv"):
        die(
            "lv_font_conv not found.\n"
            "  brew install node && npm install -g lv_font_conv"
        )
    import urllib.request
    os.makedirs(INTER_CACHE, exist_ok=True)
    if not (os.path.exists(TEXT_FONT) and os.path.exists(LABEL_FONT)):
        import zipfile
        z = os.path.join(INTER_CACHE, "inter.zip")
        urllib.request.urlretrieve(INTER_ZIP, z)
        with zipfile.ZipFile(z) as f:
            for m in ("extras/ttf/Inter-Medium.ttf", "extras/ttf/Inter-SemiBold.ttf"):
                f.extract(m, os.path.join(INTER_CACHE, "interzip"))
    if not os.path.exists(os.path.join(FONT_DIR, SYMBOL_FONT)):
        die("missing %s in %s" % (SYMBOL_FONT, FONT_DIR))


def fix_include(path):
    """Rewrite the include block lv_font_conv emits.

    It defaults to `#include "lvgl/lvgl.h"` unless LV_LVGL_H_INCLUDE_SIMPLE is
    defined. Neither of our two builds defines it, and both resolve a plain
    "lvgl.h" - the ESP-IDF component and the simulator's add_subdirectory both
    put LVGL's root on the include path. Every other file in the project
    includes it that way, so match them instead of relying on a define being
    set identically in two build systems.
    """
    with open(path, encoding="utf-8") as fh:
        src = fh.read()

    block = re.compile(
        r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\s*\n'
        r'\s*#include "lvgl\.h"\s*\n'
        r'#else\s*\n'
        r'\s*#include "lvgl/lvgl\.h"\s*\n'
        r'#endif\s*\n'
    )
    src, n = block.subn('#include "lvgl.h"\n', src)
    if n != 1:
        die("unexpected include block in %s (lv_font_conv changed?)" % path)
    # the "Opts:" line repeats the command: no path of this machine in it
    src = re.sub(r'(--font|-o) /\S*/([^/\s]+)', r'\1 \2', src)

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)


def generate(size, out_path, font=None):
    cmd = [
        "lv_font_conv",
        "--no-compress", "--no-prefilter",
        "--bpp", str(BPP),
        "--size", str(size),
        "--font", font or TEXT_FONT, "-r", text_range(size),
        "--font", SYMBOL_FONT, "-r", SYMBOL_RANGE,
        "--format", "lvgl",
        "-o", out_path,
        "--force-fast-kern-format",
    ]
    subprocess.run(cmd, check=True, cwd=FONT_DIR)
    fix_include(out_path)


def glyph_count(path):
    with open(path, encoding="utf-8") as fh:
        return len(re.findall(r"\{\.bitmap_index", fh.read()))


def bitmap_kb(path):
    """Bytes in glyph_bitmap[], which is most but not all of the flash cost.

    Match 0x0 and 0xf as well as 0xab: lv_font_conv does not zero-pad, and a
    regex that only accepts two hex digits silently drops roughly half the
    array. The authoritative figure is always the linker map, not this.
    """
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return 0.0
    return len(re.findall(r"0x[0-9a-f]{1,2}\b", m.group(1))) / 1024.0


def write_header():
    """Emit include/aos_fonts.h, derived from SIZES so it cannot drift."""
    lines = [
        "/*",
        " * P4OS font set - generated by tools/gen_fonts.py, do not edit.",
        " *",
        " * Inter Medium (and SemiBold for icon labels) with Latin-1, so accented text",
        " * renders. LVGL's own lv_font_montserrat_* are ASCII only and stay",
        " * compiled in for the dynamic apps that reference them directly.",
        " *",
        " * Do not name these fonts in UI code. Go through the role pointers",
        " * in aos_theme.h (aos_font_huge / title / body / small) so that a",
        " * language switch can repoint them in one place.",
        " */",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for s in SIZES:
        lines.append("LV_FONT_DECLARE(%s%d);" % (PREFIX, s))
    for s in LABEL_SIZES:
        lines.append("LV_FONT_DECLARE(%s%d);" % (LABEL_PREFIX, s))
    for s in NUM_SIZES:
        lines.append("LV_FONT_DECLARE(aos_inter_num_%d);" % s)
    lines += ["",
              "/* The names AmoledOS code uses. */"]
    for s in SIZES:
        lines.append("#define aos_montserrat_%d %s%d" % (s, PREFIX, s))
    lines += [
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    path = os.path.join(INC_DIR, "aos_fonts.h")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report sizes without writing into the component")
    args = ap.parse_args()

    check_tools()
    out_dir = OUT_DIR if not args.check else os.path.join("/tmp", "aos_fonts_check")
    inc_dir = INC_DIR
    os.makedirs(out_dir, exist_ok=True)
    if not args.check:
        os.makedirs(inc_dir, exist_ok=True)

    print("range : %s" % TEXT_RANGE)
    print("out   : %s\n" % out_dir)

    total_kb = 0.0
    for size in SIZES:
        name = "%s%d" % (PREFIX, size)
        path = os.path.join(out_dir, name + ".c")
        generate(size, path)
        kb = bitmap_kb(path)
        total_kb += kb
        print("  %-24s %4d glyphs  %6.1f KB bitmap" % (name, glyph_count(path), kb))
    for size in NUM_SIZES:
        name = "aos_inter_num_%d" % size
        path = os.path.join(out_dir, name + ".c")
        subprocess.run(["lv_font_conv", "--no-compress", "--no-prefilter", "--bpp", str(BPP),
                        "--size", str(size), "--font", TEXT_FONT, "-r", NUM_RANGE, "--format", "lvgl",
                        "-o", path, "--force-fast-kern-format"], check=True)
        fix_include(path)
        kb = bitmap_kb(path)
        total_kb += kb
        print("  %-24s %4d glyphs  %6.1f KB bitmap" % (name, glyph_count(path), kb))
    for size in LABEL_SIZES:
        name = "%s%d" % (LABEL_PREFIX, size)
        path = os.path.join(out_dir, name + ".c")
        generate(size, path, LABEL_FONT)
        kb = bitmap_kb(path)
        total_kb += kb
        print("  %-24s %4d glyphs  %6.1f KB bitmap" % (name, glyph_count(path), kb))

    print("\n  %-24s %19.1f KB" % ("total", total_kb))

    if args.check:
        print("\n--check: nothing written to the component")
        return

    header = write_header()
    print("  header %s" % os.path.relpath(header, REPO))
    print("\nNext: rebuild, then run tools/gen_symbols.py so the dynamic apps")
    print("can resolve the new font symbols.")


if __name__ == "__main__":
    main()
