#!/usr/bin/env python3
"""
A second source for the Tienda, to try other people's repositories with
(docs/plan/TIENDA.md, "Fuentes de terceros"): a few small apps that are
nobody's, built for the board, with their catalogue and packages laid out
the way a release is, for tools/fake_github.py to serve.

    tools/store_sample.py --out <folder> --base http://<this machine>:18392/prueba/apps/releases/download/
    tools/fake_github.py --dir <folder>/release --port 18392 --repo prueba/apps --bind <this machine>

and at the board, with developer mode on (an http:// source is a
developer's, on a trusted network), the source to add or to propose from
the portal is

    http://<this machine>:18392/prueba/apps/releases/latest/download/p4os-store.ndjson

What it makes:

    <folder>/release/p4os-store.ndjson   the catalogue of "Tienda de prueba"
    <folder>/release/<file>.p4app        its packages
    <folder>/suelta.p4app                a package that is in no catalogue, to
                                         drop on the portal's Store page or
                                         open from Files

The catalogue lists three apps. Two are its own (Contador de prueba,
Colores de prueba) and install beside the official ones; their pictures are
the template's, which is what they are. The third is
called dados.so, like the official Dados: the board must leave it out and
say that the source has one app it does not offer. The loose package is a
fourth app, App suelta.

Each app is the template (apps/hello_app) with another id, name and colours:
tapping counts and changes the background. They are written to apps/ as
prueba_*, built with tools/build_apps.sh against build/rev1_3 (the firmware
has to be built), packaged with tools/gen_store.py, and taken away again.
The first build of each takes a few minutes.

The standard library only.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = "v1.0.0"
REPO = "prueba/apps"

SOURCE = {
    "name": "Tienda de prueba",
    "by": "Nadie en particular",
    "url": "https://example.org/tienda-de-prueba",
    "about": {"es": "Una fuente inventada para probar las fuentes de terceros. Sus apps no hacen nada útil.",
              "en": "A made-up source to try third-party sources with. Its apps do nothing useful."},
}

# folder in apps/, the .so's name, id, name, the two colours of its icon,
# what it says on its screen, where it ends up
SAMPLES = (
    dict(dir="prueba_contador", file="prueba_contador", id="prueba.contador", name="Contador de prueba",
         a=0x34C759, b=0x0B6B2B, title="Vengo de otra fuente", listed=True, shots=["app-hello.png", "land-hello.png", "web-hello.png"],
         about=("Una app de muestra de una fuente de terceros: cuenta los toques.",
                "A sample app from a third-party source: it counts taps.")),
    dict(dir="prueba_colores", file="prueba_colores", id="prueba.colores", name="Colores de prueba",
         a=0xFF9F0A, b=0xB23A00, title="Otra app de la misma fuente", listed=True, shots=["land-hello.png", "app-hello.png"],
         about=("La segunda app de muestra: lo mismo, con otros colores.",
                "The second sample app: the same, in other colours.")),
    dict(dir="prueba_dados", file="dados", id="prueba.dados", name="Dados de otro",
         a=0xFF375F, b=0x8A0F2A, title="Si ves esto, algo anda mal", listed=True, shots=[],
         about=("Se llama como los Dados oficiales: la placa no la tiene que ofrecer.",
                "It is named like the official Dice: the board must not offer it.")),
    dict(dir="prueba_suelta", file="suelta", id="prueba.suelta", name="App suelta",
         a=0xBF5AF2, b=0x5B1E87, title="Me instalaron de un archivo", listed=False, shots=[],
         about=("Un paquete suelto, que no está en ningún catálogo.",
                "A loose package, in no catalogue.")),
)

CODE = r'''/*
 * A sample app for a made-up source of the Tienda (tools/store_sample.py):
 * the template with another id, name and colours. Tapping counts, and goes
 * through a few backgrounds.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"

#include <stdio.h>

/* A square with a dot in a corner: a price tag, more or less. */
static const uint8_t SAMPLE_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 56, 56, 12, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, -14, 8, 12, 12, AIC_CIRCLE, AIC_C_BG, 255),
    AIC_OUT,
    AIC_END
};

static const uint32_t BACKGROUNDS[] = { 0x101018, 0x%(b)06X, 0x1C1C1E, 0x%(a)06X };

typedef struct {
    lv_obj_t *root;
    lv_obj_t *counter;
    int       taps;
} sample_t;

static void tap_cb(lv_event_t *event)
{
    sample_t *s = (sample_t *)lv_event_get_user_data(event);
    s->taps++;
    char buf[32];
    snprintf(buf, sizeof buf, "%%d", s->taps);
    lv_label_set_text(s->counter, buf);
    lv_obj_set_style_bg_color(s->root, lv_color_hex(BACKGROUNDS[s->taps %% 4]), 0);
    aos_hal_beep(1200 + 100 * (s->taps %% 4), 30);
}

static void *sample_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    sample_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) return NULL;
    s->root = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(BACKGROUNDS[0]), 0);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, _("%(title)s"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s->counter = lv_label_create(root);
    lv_label_set_text(s->counter, "0");
    lv_obj_set_style_text_color(s->counter, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s->counter, &aos_montserrat_48, 0);
    lv_obj_center(s->counter);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, _("tocar para sumar"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0xC7C7CC), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, tap_cb, LV_EVENT_CLICKED, s);
    return s;
}

static void sample_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    lv_free(inst);
}

static bool sample_init(aos_app_t *app)
{
    app->desc.id       = "%(id)s";
    app->desc.name     = "%(name)s";
    app->desc.icon     = LV_SYMBOL_OK;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x%(a)06X;
    app->desc.color_b  = 0x%(b)06X;
    app->desc.order    = 900;
    aos_icon_set_ops(app, SAMPLE_ICON, sizeof SAMPLE_ICON);

    app->create  = sample_create;
    app->destroy = sample_destroy;
    return true;
}

AOS_APP_ENTRY(sample_init);
'''

PROJECT = '''cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(%(file)s)

include(${CMAKE_CURRENT_LIST_DIR}/../common.cmake)

include(elf_loader)
project_so(%(file)s)
'''

MAIN = '''idf_component_register(
    SRCS "%(file)s.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
'''


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def lay_out(s):
    """apps/<dir>: the template's project files, this app's code and card."""
    d = os.path.join(ROOT, "apps", s["dir"])
    tpl = os.path.join(ROOT, "apps", "hello_app")
    write(os.path.join(d, "CMakeLists.txt"), PROJECT % s)
    write(os.path.join(d, "main", "CMakeLists.txt"), MAIN % s)
    write(os.path.join(d, "main", s["file"] + ".c"), CODE % s)
    shutil.copy(os.path.join(tpl, "main", "idf_component.yml"), os.path.join(d, "main"))
    for f in ("sdkconfig.defaults", "dependencies.lock"):
        shutil.copy(os.path.join(tpl, f), d)
    card = {"cat": "utiles", "about": {"es": s["about"][0], "en": s["about"][1]}}
    if s["shots"]:
        card["shots"] = s["shots"]
    write(os.path.join(d, "store.json"), json.dumps(card, ensure_ascii=False, indent=1) + "\n")
    return d


def main():
    ap = argparse.ArgumentParser(description="A made-up second source for the Tienda, to test with.")
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", required=True,
                    help="where its files will be served from: http://<host>:<port>/%s/releases/download/" % REPO)
    ap.add_argument("--keep", action="store_true", help="leave the apps/prueba_* folders, built, for another run")
    a = ap.parse_args()
    if not a.base.endswith("/releases/download/"):
        sys.exit("--base ends in /releases/download/")

    taken = [s["dir"] for s in SAMPLES if os.path.exists(os.path.join(ROOT, "apps", s["dir"], "main"))
             and not os.path.exists(os.path.join(ROOT, "apps", s["dir"], ".store_sample"))]
    if taken:
        sys.exit("apps/%s is there already and is not this tool's" % taken[0])

    release = os.path.join(a.out, "release")
    if os.path.isdir(release):
        shutil.rmtree(release)
    os.makedirs(release)
    dirs = []
    try:
        for s in SAMPLES:
            dirs.append(lay_out(s))
            write(os.path.join(dirs[-1], ".store_sample"), "made by tools/store_sample.py: safe to delete\n")
        names = [s["dir"] for s in SAMPLES]
        print("building %s..." % ", ".join(names), flush=True)
        r = subprocess.run([os.path.join(ROOT, "tools", "build_apps.sh")] + names, cwd=ROOT)
        missing = [s["dir"] for s in SAMPLES
                   if not os.path.exists(os.path.join(ROOT, "apps", s["dir"], "build", s["file"] + ".so"))]
        if r.returncode or missing:
            sys.exit("the build failed%s" % (": " + ", ".join(missing) if missing else ""))
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_store.py"), "build", "--release", RELEASE,
                        "--out", release, "--only", ",".join(names), "--repo", REPO, "--base", a.base], cwd=ROOT, check=True)
    finally:
        if not a.keep:
            for d in dirs:
                shutil.rmtree(d, ignore_errors=True)

    # the catalogue is this source's and not P4OS's, and the loose package is in none
    cat = os.path.join(release, "p4os-store.ndjson")
    lines = [json.loads(l) for l in open(cat).read().splitlines() if l.strip()]
    head, apps = lines[0], lines[1:]
    head["source"] = SOURCE
    loose = {s["file"] for s in SAMPLES if not s["listed"]}
    with open(cat, "w") as f:
        for x in [head] + [x for x in apps if x["file"] not in loose]:
            f.write(json.dumps(x, ensure_ascii=False, separators=(",", ":")) + "\n")
    for file in loose:
        shutil.move(os.path.join(release, file + ".p4app"), os.path.join(a.out, file + ".p4app"))

    print("\n%s: %d apps in its catalogue (%s)" % (SOURCE["name"], len(apps) - len(loose),
                                                 ", ".join(x["file"] for x in apps if x["file"] not in loose)))
    for file in sorted(loose):
        print("a loose package: %s" % os.path.join(a.out, file + ".p4app"))
    host = a.base.split("/")[2]
    print("serve it:   tools/fake_github.py --dir %s --port %s --repo %s --bind %s" %
          (release, host.split(":")[1] if ":" in host else "80", REPO, host.split(":")[0]))
    print("its source: %s" % a.base.replace("/releases/download/", "/releases/latest/download/p4os-store.ndjson"))


if __name__ == "__main__":
    main()
