#!/usr/bin/env python3
"""
Compare system typefaces in the simulator.

    tools/font_preview.py ~/.cache/p4os-fonts/cand/*.ttf

For each TTF: LVGL binary fonts at the shell's sizes into
sim/sim_fs/fonts/<name>/, then the simulator is run with P4_SIM_FONT=<name>
to take the same screens, and bench/results/fonts/ gets one sheet with every
typeface side by side plus a 1:1 crop of the icon labels.
"""
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(REPO, "sim")
SIZES = [16, 20, 24, 28, 36, 48, 64]
RANGE = "0x20-0x7F,0xA0-0xFF,0x2022,0x2026,0x20AC"
OUT = os.path.join(REPO, "bench", "results", "fonts")


def main():
    from PIL import Image, ImageDraw
    os.makedirs(OUT, exist_ok=True)
    fonts = sys.argv[1:]
    shots = []
    for ttf in ["(montserrat actual)"] + fonts:
        name = "current" if ttf.startswith("(") else os.path.splitext(os.path.basename(ttf))[0]
        env = dict(os.environ)
        if name != "current":
            d = os.path.join(SIM, "sim_fs", "fonts", name)
            os.makedirs(d, exist_ok=True)
            for px in SIZES:
                subprocess.run(["lv_font_conv", "--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(px),
                                "--font", ttf, "-r", RANGE, "--format", "bin", "-o", os.path.join(d, "%d.bin" % px)],
                               check=True, capture_output=True)
            env["P4_SIM_FONT"] = name
        home = os.path.join(OUT, name + "_home.png")
        app = os.path.join(OUT, name + "_app.png")
        prefs = os.path.join(SIM, "sim_fs", "prefs.txt")
        if os.path.exists(prefs):
            os.remove(prefs)
        env["P4_SIM_SCRIPT"] = "wait 900; shot %s; open aos.ha; wait 700; shot %s; quit" % (home, app)
        subprocess.run([os.path.join(SIM, "build", "p4os_sim")], cwd=SIM, env=env, capture_output=True, timeout=60)
        shots.append((name, home, app))
        print("  ", name)

    # sheet 1: the home screens; sheet 2: icon labels 1:1; sheet 3: a text screen
    def sheet(path, items, crop=None, scale=0.5):
        ims = []
        for label, f in items:
            im = Image.open(f).convert("RGB")
            if crop:
                im = im.crop(crop)
            im = im.resize((int(im.width * scale), int(im.height * scale)))
            ims.append((label, im))
        cols = 4
        cw = max(i.width for _, i in ims) + 10
        ch = max(i.height for _, i in ims) + 34
        rows = (len(ims) + cols - 1) // cols
        s = Image.new("RGB", (cols * cw, rows * ch), (30, 30, 30))
        d = ImageDraw.Draw(s)
        for k, (label, im) in enumerate(ims):
            x, y = (k % cols) * cw, (k // cols) * ch
            s.paste(im, (x + 5, y + 30))
            d.text((x + 8, y + 8), label, fill=(255, 255, 255))
        s.save(path)
        print("  ->", os.path.relpath(path, REPO))
    sheet(os.path.join(OUT, "sheet_home.png"), [(n, h) for n, h, _ in shots], scale=0.4)
    sheet(os.path.join(OUT, "sheet_labels.png"), [(n, h) for n, h, _ in shots], crop=(0, 380, 720, 900), scale=0.5)
    sheet(os.path.join(OUT, "sheet_text.png"), [(n, a) for n, _, a in shots], crop=(0, 380, 720, 900), scale=0.5)


if __name__ == "__main__":
    main()
