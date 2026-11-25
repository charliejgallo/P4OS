#!/usr/bin/env python3
"""The samples on the real home screen: a pack per style, the simulator,
screenshots.

    python3 tools/icons/sim_shots.py [a_plano b_chubby c_linea]

For each style it builds a pack with tools/icons/mkpack.py from
muestra/<style>/ (<id>_120.png as the master, <id>_108.png and <id>_26.png
as the sizes made by hand) and runs sim/build/p4os_sim twice, scripted:

  grid    the clock widget and the nine icons in the 4-column grid, five of
          them in the dock; then the same lying down (108 px)
  folder  the nine inside one folder (its 3x3 of 26 px miniatures), and the
          folder open

An app shows once in menu.txt, so the grid and the folder are two runs.

It never touches sim/sim_fs: each run happens in its own folder under
muestra/sim/, with a sim_fs made of links to every entry of the main
checkout's sim/sim_fs except icons/ (the pack) and menu.txt, and its own
prefs and ports. The simulator OPENS A WINDOW: not while the user is in a
call or sharing the screen.
"""

import os
import shutil
import subprocess
import sys

import common as C

ROOT = os.path.abspath(os.path.join(C.HERE, "..", ".."))
SIM_BIN = os.path.join(ROOT, "sim", "build", "p4os_sim")
# The shared fake card lives in the main checkout (a worktree has no data).
SIM_FS = os.environ.get("P4_SIM_FS_SRC", os.path.join(ROOT, "sim", "sim_fs"))
if not os.path.isdir(os.path.join(SIM_FS, "apps")):
    SIM_FS = "/Users/charlie/Dropbox/ClaudeCode/P4OS/sim/sim_fs"
RUNS = os.path.join(C.OUT, "sim")

NINE = [a for a, _n, _i in C.ALL]

MENU_GRID = """dock
  app aos.settings
  app aos.store
  app aos.notas
  app aos.clock
end
widget clock 4x2
app aos.clima
app demo.metegol
app aos.serial
app aos.calc
app aos.photos
page
"""

MENU_FOLDER = ("dock\n  app aos.files\n  app aos.music\n  app aos.ha\n  app aos.bus\nend\n"
               "widget clock 4x2\n"
               "folder muestra 5E6B80 3A4150 v apps w Muestra\n"
               + "".join(f"  app {a}\n" for a in NINE) +
               "end\napp aos.flasher\napp aos.cameras\napp aos.claude\napp aos.macropad\n"
               "app aos.sysmon\napp aos.cotiz\napp aos.radio\npage\n")

PHOTO = "sim_fs/photos/IMG_0004.jpg"

# Where the first grid cell is (the folder in MENU_FOLDER): the clock widget
# takes the first two rows of the grid.
FOLDER_TAP = (108, 460)


def make_pack(style, run):
    src = os.path.join(run, "pack_src")
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    for a in NINE:
        shutil.copy(C.out_path(style, a, 120), os.path.join(src, f"{a}.png"))
        shutil.copy(C.out_path(style, a, 108), os.path.join(src, f"{a}@108.png"))
        shutil.copy(C.out_path(style, a, 26), os.path.join(src, f"{a}@26.png"))
    os.makedirs(os.path.join(run, "sim_fs", "icons"), exist_ok=True)
    pak = os.path.join(run, "sim_fs", "icons", "hd.pak")
    subprocess.run([sys.executable, os.path.join(C.HERE, "mkpack.py"), src, pak], check=True)
    return pak


def make_fs(run, menu, extra_prefs=()):
    fs = os.path.join(run, "sim_fs")
    os.makedirs(fs, exist_ok=True)
    for name in os.listdir(SIM_FS):
        if name in ("icons", "menu.txt", "prefs.txt"):
            continue
        dst = os.path.join(fs, name)
        if not os.path.lexists(dst):
            os.symlink(os.path.join(SIM_FS, name), dst)
    with open(os.path.join(fs, "menu.txt"), "w") as f:
        f.write(menu)
    prefs = os.path.join(run, "prefs.txt")
    with open(os.path.join(SIM_FS, "prefs.txt")) as f:
        lines = [l for l in f if not l.startswith("landscape=")]
    with open(prefs, "w") as f:
        f.writelines(lines + ["landscape=0\n"] + [p + "\n" for p in extra_prefs])  # upright
    return prefs


def run_sim(run, prefs, script):
    env = dict(os.environ, P4_SIM_PREFS=prefs, P4_SIM_PORTAL_PORT="18093",
               P4_SIM_SSH_PORT="12293", P4_SIM_ZOOM="0.4", P4_SIM_SCRIPT=script)
    subprocess.run([SIM_BIN], cwd=run, env=env, timeout=120,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main(styles):
    for style in styles:
        for kind, menu, prefs_add, script in (
            ("grid", MENU_GRID, (),
             "wait 2500; shot home.png; rotate; wait 1500; shot land.png; quit"),
            ("folder", MENU_FOLDER, (),
             f"wait 2500; shot home.png; tap {FOLDER_TAP[0]} {FOLDER_TAP[1]}; wait 1200; "
             "shot open.png; quit"),
            # over a photo, like the board's own home screen
            ("photo", MENU_GRID, (f"wall_img={PHOTO}",), "wait 4500; shot home.png; quit"),
        ):
            run = os.path.join(RUNS, style, kind)
            os.makedirs(run, exist_ok=True)
            make_pack(style, run)
            prefs = make_fs(run, menu, prefs_add)
            run_sim(run, prefs, script)
            print(style, kind, sorted(f for f in os.listdir(run) if f.endswith(".png")))


if __name__ == "__main__":
    main(sys.argv[1:] or ["a_plano", "b_chubby", "c_linea"])
