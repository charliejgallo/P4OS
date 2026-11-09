/*
 * P4OS - Luces: the library, the show and the engine (lz.h says the model).
 *
 * A type is a CSV file in <card>/luces/aparatos, two lines:
 *
 *   nombre,PAR RGB de 7 canales
 *   canales,dimmer,rojo,verde,azul,strobe,0,0
 *
 * one word a channel, in order: dimmer, rojo, verde, azul, blanco, ambar,
 * uv, strobe, pan, pan_fino, tilt, tilt_fino, or a number (a channel held
 * at that value: a macro at 0, a mode at 255), or "algo=valor" (the same,
 * with a name for whoever reads the file). The built-in types are written
 * the same way below. '#' starts a comment.
 *
 * The patch is luces/patch.csv (name, type name, address); the scenes,
 * luces/escenas.txt, a line each: name|fade ms|i:dim,r,g,b,w,amber,uv,
 * strobe,pan,tilt;i:... with i the fixture's place in the patch.
 */
#include "lz.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

lz_t LZ;

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static const char *const ATTR_WORD[LZ_A_COUNT] = { "dimmer", "rojo", "verde", "azul", "blanco", "ambar", "uv", "strobe",
                                                   "pan", "pan_fino", "tilt", "tilt_fino", "fijo" };

const char *lz_attr_name(int a) { return a >= 0 && a < LZ_A_COUNT ? ATTR_WORD[a] : "?"; }

static const char *const BUILTIN[][2] = {
    { "Dimmer", "dimmer" },
    { "RGB (3 canales)", "rojo,verde,azul" },
    { "RGBW (4 canales)", "rojo,verde,azul,blanco" },
    { "PAR RGB de 7 canales", "dimmer,rojo,verde,azul,strobe,macro=0,velocidad=0" },
    { "PAR RGBWA+UV (6 canales)", "rojo,verde,azul,blanco,ambar,uv" },
    { "Cabeza móvil (6 canales)", "pan,tilt,dimmer,rojo,verde,azul" },
    { "Cabeza móvil (9 canales)", "pan,pan_fino,tilt,tilt_fino,velocidad=0,dimmer,rojo,verde,azul" },
    { "Strobe (2 canales)", "dimmer,strobe" },
    { "Máquina de humo", "dimmer" },
};

/* ---- the folder ---- */

const char *lz_dir(void)
{
    static char dir[96];
    const char *root = aos_hal_path_sd_root();
    snprintf(dir, sizeof dir, "%s/luces", root ? root : aos_hal_path_data());
    mkdir(dir, 0777);
    char sub[120];
    snprintf(sub, sizeof sub, "%s/aparatos", dir);
    mkdir(sub, 0777);
    return dir;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* ---- types ---- */

static bool parse_channels(lz_type_t *t, const char *list)
{
    char buf[400];
    snprintf(buf, sizeof buf, "%s", list);
    t->n = 0;
    for (char *p = buf, *next; p && *p && t->n < LZ_CH; p = next) {
        next = strchr(p, ',');
        if (next) *next++ = 0;
        char *w = trim(p);
        for (char *c = w; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');      /* tolower: not exported */
        int k = t->n;
        char *eq = strchr(w, '=');
        if (eq || isdigit((unsigned char)w[0])) {
            t->attr[k] = LZ_A_CONST;
            long v = strtol(eq ? eq + 1 : w, NULL, 0);
            t->def[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        } else {
            int a = -1;
            for (int i = 0; i < LZ_A_CONST; i++) if (!strcmp(w, ATTR_WORD[i])) a = i;
            if (!strcmp(w, "intensidad")) a = LZ_A_DIM;
            if (!strcmp(w, "ámbar")) a = LZ_A_AMBER;
            if (a < 0) { t->attr[k] = LZ_A_CONST; t->def[k] = 0; }      /* unknown: held at 0 */
            else t->attr[k] = (uint8_t)a;
        }
        t->n++;
    }
    for (int k = 0; k < t->n; k++) {
        int a = t->attr[k];
        t->has_dim |= a == LZ_A_DIM;
        t->has_color |= a >= LZ_A_R && a <= LZ_A_UV;
        t->has_w |= a == LZ_A_W;
        t->has_pos |= a >= LZ_A_PAN && a <= LZ_A_TILT_F;
        t->has_strobe |= a == LZ_A_STROBE;
    }
    return t->n > 0;
}

static void type_add(const char *name, const char *channels, const char *file)
{
    if (LZ.ntypes >= LZ_TYPES) return;
    lz_type_t *t = &LZ.types[LZ.ntypes];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    snprintf(t->file, sizeof t->file, "%s", file ? file : "");
    if (parse_channels(t, channels)) LZ.ntypes++;
}

static void type_file(const char *path, const char *file)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[400], name[40] = "", chans[400] = "";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        if (!strncasecmp(s, "nombre,", 7)) snprintf(name, sizeof name, "%.39s", trim(s + 7));
        else if (!strncasecmp(s, "canales,", 8)) snprintf(chans, sizeof chans, "%s", s + 8);
    }
    fclose(f);
    if (chans[0]) type_add(name[0] ? name : file, chans, file);
}

void lz_types_load(void)
{
    LZ.ntypes = 0;
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) type_add(BUILTIN[i][0], BUILTIN[i][1], NULL);
    char dir[140];
    snprintf(dir, sizeof dir, "%s/aparatos", lz_dir());
    DIR *d = opendir(dir);
    bool any = false;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".csv")) continue;
            char p[200];
            snprintf(p, sizeof p, "%.130s/%.60s", dir, e->d_name);
            type_file(p, e->d_name);
            any = true;
        }
        closedir(d);
    }
    if (!any) {                         /* an example to copy from */
        char p[200];
        snprintf(p, sizeof p, "%s/ejemplo-par.csv", dir);
        FILE *f = fopen(p, "w");
        if (f) {
            fputs("# P4OS Luces: un tipo de aparato (apps/luces/README.md)\n"
                  "# Una palabra por canal, en orden: dimmer, rojo, verde, azul, blanco, ambar, uv,\n"
                  "# strobe, pan, pan_fino, tilt, tilt_fino, o un numero (el canal queda fijo en ese\n"
                  "# valor), o algo=valor (lo mismo, con nombre). Lo dice el manual del aparato.\n"
                  "nombre,PAR de ejemplo (8 canales)\n"
                  "canales,dimmer,rojo,verde,azul,blanco,strobe,macro=0,velocidad=0\n", f);
            fclose(f);
            type_file(p, "ejemplo-par.csv");
        }
    }
}

/* ---- the patch ---- */

static int type_by_name(const char *name)
{
    for (int i = 0; i < LZ.ntypes; i++) if (!strcmp(LZ.types[i].name, name)) return i;
    return -1;
}

int lz_types_reload(void)
{
    static char was[LZ_FIX][40];
    for (int i = 0; i < LZ.nfix; i++) snprintf(was[i], sizeof was[i], "%s", LZ.types[LZ.fix[i].type].name);
    lz_types_load();
    int lost = 0;
    for (int i = 0; i < LZ.nfix; i++) {
        int t = type_by_name(was[i]);
        if (t < 0) { t = 0; lost++; aos_hal_log("luces", "type %s is gone; %s is a dimmer now", was[i], LZ.fix[i].name); }
        LZ.fix[i].type = t;
    }
    LZ.changed++;
    if (lost) lz_patch_save();
    return lost;
}

static void look_default(lz_look_t *l)
{
    memset(l, 0, sizeof *l);
    l->r = l->g = l->b = 255;
    l->pan = l->tilt = 32768;
}

int lz_fix_add(int type, uint16_t addr, const char *name)
{
    if (LZ.nfix >= LZ_FIX || type < 0 || type >= LZ.ntypes || addr < 1 || addr > 512) return -1;
    int i = LZ.nfix++;
    lz_fix_t *f = &LZ.fix[i];
    f->type = type;
    f->addr = addr;
    if (name && name[0]) snprintf(f->name, sizeof f->name, "%s", name);
    else snprintf(f->name, sizeof f->name, "%.18s %u", LZ.types[type].name, (unsigned)(uint8_t)(i + 1));
    look_default(&LZ.look[i]);
    LZ.from[i] = LZ.shown[i] = LZ.look[i];
    LZ.sel[i] = false;
    LZ.fx_on[i] = false;
    LZ.changed++;
    return i;
}

void lz_fix_remove(int i)
{
    if (i < 0 || i >= LZ.nfix) return;
    int n = LZ.nfix - i - 1;
    memmove(&LZ.fix[i], &LZ.fix[i + 1], (size_t)n * sizeof LZ.fix[0]);
    memmove(&LZ.look[i], &LZ.look[i + 1], (size_t)n * sizeof LZ.look[0]);
    memmove(&LZ.from[i], &LZ.from[i + 1], (size_t)n * sizeof LZ.from[0]);
    memmove(&LZ.shown[i], &LZ.shown[i + 1], (size_t)n * sizeof LZ.shown[0]);
    memmove(&LZ.sel[i], &LZ.sel[i + 1], (size_t)n * sizeof LZ.sel[0]);
    memmove(&LZ.fx_on[i], &LZ.fx_on[i + 1], (size_t)n * sizeof LZ.fx_on[0]);
    for (int s = 0; s < LZ.nscenes && LZ.scenes; s++)
        memmove(&LZ.scenes[s].look[i], &LZ.scenes[s].look[i + 1], (size_t)n * sizeof LZ.look[0]);
    LZ.nfix--;
    LZ.changed++;
}

int lz_next_free(int nch)
{
    for (int a = 1; a + nch - 1 <= 512; a++) {
        bool ok = true;
        for (int i = 0; i < LZ.nfix && ok; i++) {
            int b = LZ.fix[i].addr, e = b + LZ.types[LZ.fix[i].type].n - 1;
            if (a <= e && a + nch - 1 >= b) { ok = false; a = e; }
        }
        if (ok) return a;
    }
    return -1;
}

bool lz_fix_overlaps(int i)
{
    int b = LZ.fix[i].addr, e = b + LZ.types[LZ.fix[i].type].n - 1;
    if (e > 512) return true;
    for (int j = 0; j < LZ.nfix; j++) {
        if (j == i) continue;
        int b2 = LZ.fix[j].addr, e2 = b2 + LZ.types[LZ.fix[j].type].n - 1;
        if (b <= e2 && e >= b2 && !(b == b2 && LZ.fix[j].type == LZ.fix[i].type)) return true;
    }
    return false;
}

bool lz_patch_save(void)
{
    char p[140], tmp[150];
    snprintf(p, sizeof p, "%s/patch.csv", lz_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# P4OS Luces: el patch (nombre,tipo,direccion)\n", f);
    for (int i = 0; i < LZ.nfix; i++) fprintf(f, "%s,%s,%u\n", LZ.fix[i].name, LZ.types[LZ.fix[i].type].name, LZ.fix[i].addr);
    bool ok = fclose(f) == 0;
    if (ok) { remove(p); ok = rename(tmp, p) == 0; }
    return ok;
}

/* ---- scenes ---- */

static void *big(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
#else
    return calloc(1, n);
#endif
}

bool lz_scenes_save(void)
{
    char p[140], tmp[150];
    snprintf(p, sizeof p, "%s/escenas.txt", lz_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("# P4OS Luces: las escenas (nombre|fundido ms|i:dim,r,g,b,w,ambar,uv,strobe,pan,tilt;...)\n", f);
    for (int s = 0; s < LZ.nscenes; s++) {
        lz_scene_t *sc = &LZ.scenes[s];
        fprintf(f, "%s|%u|", sc->name, sc->fade_ms);
        for (int i = 0; i < LZ.nfix; i++) {
            lz_look_t *l = &sc->look[i];
            fprintf(f, "%s%d:%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", i ? ";" : "", i, l->dim, l->r, l->g, l->b, l->w, l->amber, l->uv, l->strobe, l->pan, l->tilt);
        }
        fputc('\n', f);
    }
    bool ok = fclose(f) == 0;
    if (ok) { remove(p); ok = rename(tmp, p) == 0; }
    return ok;
}

static void scenes_load(void)
{
    LZ.nscenes = 0;
    char p[140];
    snprintf(p, sizeof p, "%s/escenas.txt", lz_dir());
    FILE *f = fopen(p, "r");
    if (!f) return;
    static char line[LZ_FIX * 48 + 128];
    while (fgets(line, sizeof line, f) && LZ.nscenes < LZ_SCENES) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *b1 = strchr(s, '|');
        if (!b1) continue;
        *b1 = 0;
        char *b2 = strchr(b1 + 1, '|');
        if (!b2) continue;
        *b2 = 0;
        lz_scene_t *sc = &LZ.scenes[LZ.nscenes];
        memset(sc, 0, sizeof *sc);
        snprintf(sc->name, sizeof sc->name, "%.23s", s);
        sc->fade_ms = (uint16_t)atoi(b1 + 1);
        for (int i = 0; i < LZ_FIX; i++) look_default(&sc->look[i]);
        for (char *q = b2 + 1, *next; q && *q; q = next) {
            next = strchr(q, ';');
            if (next) *next++ = 0;
            unsigned v[11];
            if (sscanf(q, "%u:%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) == 11 &&
                v[0] < LZ_FIX) {
                lz_look_t *l = &sc->look[v[0]];
                l->dim = (uint8_t)v[1]; l->r = (uint8_t)v[2]; l->g = (uint8_t)v[3]; l->b = (uint8_t)v[4]; l->w = (uint8_t)v[5];
                l->amber = (uint8_t)v[6]; l->uv = (uint8_t)v[7]; l->strobe = (uint8_t)v[8]; l->pan = (uint16_t)v[9]; l->tilt = (uint16_t)v[10];
            }
        }
        LZ.nscenes++;
    }
    fclose(f);
}

void lz_show_load(void)
{
    if (!LZ.scenes) LZ.scenes = big(sizeof(lz_scene_t) * LZ_SCENES);
    LZ.nfix = 0;
    char p[140];
    snprintf(p, sizeof p, "%s/patch.csv", lz_dir());
    FILE *f = fopen(p, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            char *c1 = strchr(s, ','), *c2 = c1 ? strrchr(c1 + 1, ',') : NULL;
            if (!c1 || !c2) continue;
            *c1 = 0;
            *c2 = 0;
            int t = type_by_name(trim(c1 + 1));
            if (t >= 0) lz_fix_add(t, (uint16_t)atoi(c2 + 1), trim(s));
            else aos_hal_log("luces", "patch: no type called %s", trim(c1 + 1));
        }
        fclose(f);
    } else {
        /* a first show to look at: the bench's strip as eight RGB pixels */
        for (int k = 0; k < 8; k++) {
            char n[24];
            snprintf(n, sizeof n, "Pixel %d", k + 1);
            lz_fix_add(1, (uint16_t)(1 + 3 * k), n);
        }
        lz_fix_add(3, 25, "PAR 1");
        lz_patch_save();
    }
    if (LZ.scenes) {
        snprintf(p, sizeof p, "%s/escenas.txt", lz_dir());
        struct stat sb;
        if (stat(p, &sb) == 0) scenes_load();
        else lz_scenes_demo();                  /* a first show: something for the chase to go through */
    }
    LZ.cur_scene = -1;
}

int lz_scene_store(const char *name)
{
    if (!LZ.scenes || LZ.nscenes >= LZ_SCENES) return -1;
    lz_scene_t *sc = &LZ.scenes[LZ.nscenes];
    memset(sc, 0, sizeof *sc);
    if (name && name[0]) snprintf(sc->name, sizeof sc->name, "%.23s", name);
    else snprintf(sc->name, sizeof sc->name, "%d", LZ.nscenes + 1);
    sc->fade_ms = LZ.scene_fade_ms;
    memcpy(sc->look, LZ.look, sizeof LZ.look);
    LZ.cur_scene = LZ.nscenes;
    LZ.nscenes++;
    LZ.changed++;
    lz_scenes_save();
    return LZ.cur_scene;
}

static uint8_t lerp8(uint8_t a, uint8_t b, float k) { return (uint8_t)(a + (b - a) * k + 0.5f); }
static uint16_t lerp16(uint16_t a, uint16_t b, float k) { return (uint16_t)(a + (b - a) * k + 0.5f); }

/* how far the fade is, 0..1 (1: none running) */
static float fade_k(void)
{
    if (!LZ.fade_ms) return 1.0f;
    float k = (float)(now_ms() - LZ.fade_t0) / (float)LZ.fade_ms;
    return k >= 1.0f ? 1.0f : k;
}

/* the looks as the fade has them now, before any effect */
static void base(lz_look_t *out)
{
    float k = fade_k();
    for (int i = 0; i < LZ.nfix; i++) {
        const lz_look_t *a = &LZ.from[i], *b = &LZ.look[i];
        lz_look_t *s = &out[i];
        if (k >= 1.0f) { *s = *b; continue; }
        s->dim = lerp8(a->dim, b->dim, k); s->r = lerp8(a->r, b->r, k); s->g = lerp8(a->g, b->g, k); s->b = lerp8(a->b, b->b, k);
        s->w = lerp8(a->w, b->w, k); s->amber = lerp8(a->amber, b->amber, k); s->uv = lerp8(a->uv, b->uv, k);
        s->strobe = k < 0.5f ? a->strobe : b->strobe;
        s->pan = lerp16(a->pan, b->pan, k); s->tilt = lerp16(a->tilt, b->tilt, k);
    }
}

/* where the looks are now (mid-fade too): the start of the next fade */
static void freeze(void)
{
    base(LZ.from);
    LZ.fade_ms = 0;
}

void lz_scene_recall(int i, uint16_t fade_ms)
{
    if (!LZ.scenes || i < 0 || i >= LZ.nscenes) return;
    freeze();
    memcpy(LZ.look, LZ.scenes[i].look, sizeof LZ.look);
    LZ.fade_t0 = now_ms();
    LZ.fade_ms = fade_ms;
    LZ.cur_scene = i;
}

void lz_scene_delete(int i)
{
    if (!LZ.scenes || i < 0 || i >= LZ.nscenes) return;
    memmove(&LZ.scenes[i], &LZ.scenes[i + 1], (size_t)(LZ.nscenes - i - 1) * sizeof LZ.scenes[0]);
    LZ.nscenes--;
    if (LZ.cur_scene >= LZ.nscenes) LZ.cur_scene = -1;
    LZ.changed++;
    lz_scenes_save();
}

void lz_tap(void)
{
    uint32_t t = now_ms();
    if (LZ.ntaps && t - LZ.taps[LZ.ntaps - 1] > 2500) LZ.ntaps = 0;      /* a pause starts a new count */
    if (LZ.ntaps == 4) { memmove(LZ.taps, LZ.taps + 1, 3 * sizeof LZ.taps[0]); LZ.ntaps = 3; }
    LZ.taps[LZ.ntaps++] = t;
    if (LZ.ntaps >= 2) {
        uint32_t span = (LZ.taps[LZ.ntaps - 1] - LZ.taps[0]) / (uint32_t)(LZ.ntaps - 1);
        if (span) {
            uint32_t bpm = 60000 / span;
            LZ.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
        }
    }
}

/* ---- the programmer ---- */

static void each_selected(void (*fn)(lz_look_t *l, const void *arg), const void *arg)
{
    base(LZ.look);                                  /* a fade in progress stops where it is */
    LZ.fade_ms = 0;
    bool any = false;
    for (int i = 0; i < LZ.nfix; i++) any |= LZ.sel[i];
    for (int i = 0; i < LZ.nfix; i++) if (LZ.sel[i] || !any) fn(&LZ.look[i], arg);     /* none selected: all */
    memcpy(LZ.from, LZ.look, sizeof LZ.from);
}

static void do_dim(lz_look_t *l, const void *a) { l->dim = *(const uint8_t *)a; }
static void do_rgb(lz_look_t *l, const void *a) { const uint8_t *c = a; l->r = c[0]; l->g = c[1]; l->b = c[2]; l->w = c[3]; l->amber = 0; l->uv = 0; }
static void do_strobe(lz_look_t *l, const void *a) { l->strobe = *(const uint8_t *)a; }
static void do_pos(lz_look_t *l, const void *a) { const uint16_t *p = a; l->pan = p[0]; l->tilt = p[1]; }

void lz_set_dim(uint8_t v) { each_selected(do_dim, &v); }
void lz_set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t w) { uint8_t c[4] = { r, g, b, w }; each_selected(do_rgb, c); }
void lz_set_strobe(uint8_t v) { each_selected(do_strobe, &v); }
void lz_set_pos(uint16_t pan, uint16_t tilt) { uint16_t p[2] = { pan, tilt }; each_selected(do_pos, p); }

void lz_effect(int fx)
{
    LZ.fx = fx;
    memcpy(LZ.fx_on, LZ.sel, sizeof LZ.fx_on);
    bool any = false;
    for (int i = 0; i < LZ.nfix; i++) any |= LZ.fx_on[i];
    if (!any) for (int i = 0; i < LZ.nfix; i++) LZ.fx_on[i] = true;     /* none selected: all of them */
}

/* ---- the engine ---- */

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }     /* fminf, fmaxf: not exported */

static void hue(float h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float x = h - floorf(h);
    float R = fabsf(x * 6.0f - 3.0f) - 1.0f, G = 2.0f - fabsf(x * 6.0f - 2.0f), B = 2.0f - fabsf(x * 6.0f - 4.0f);
    *r = (uint8_t)(clamp01(R) * 255);
    *g = (uint8_t)(clamp01(G) * 255);
    *b = (uint8_t)(clamp01(B) * 255);
}

static uint32_t s_rng = 0x1234567;
static uint32_t rng(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

void lz_tick(void)
{
    uint32_t now = now_ms();
    /* the chase: every scene in turn, at the tempo */
    if (LZ.chase && LZ.nscenes > 0 && LZ.bpm) {
        uint32_t period = 60000u / LZ.bpm;
        if (now - LZ.chase_t >= period) {
            LZ.chase_t = now;
            lz_scene_recall((LZ.cur_scene + 1) % LZ.nscenes, (uint16_t)(period * LZ.chase_fade / 100));
        }
    }
    /* the fade */
    base(LZ.shown);
    if (fade_k() >= 1.0f) { LZ.fade_ms = 0; memcpy(LZ.from, LZ.look, sizeof LZ.from); }
    /* the effect */
    if (LZ.fx) {
        int n = 0, j = 0;
        for (int i = 0; i < LZ.nfix; i++) n += LZ.fx_on[i];
        float t = (float)now / 1000.0f * (0.05f + LZ.fx_speed / 50.0f);
        for (int i = 0; i < LZ.nfix; i++) {
            if (!LZ.fx_on[i]) continue;
            lz_look_t *s = &LZ.shown[i];
            float ph = n ? (float)j / (float)n : 0;
            j++;
            switch (LZ.fx) {
            case LZ_FX_RAINBOW:
                hue(t * 0.25f + ph, &s->r, &s->g, &s->b);
                s->w = 0;
                if (!s->dim) s->dim = 255;
                break;
            case LZ_FX_WAVE:
                s->dim = (uint8_t)(s->dim * (0.5f + 0.5f * sinf(6.2831853f * (t * 0.5f - ph))));
                break;
            case LZ_FX_SPARKLE:
                s->dim = (rng() % 100) < 12 ? (s->dim ? s->dim : 255) : 0;
                break;
            /* Test modes. Recorrido lights one fixture at a time, full
             * white: which one is which, and an address set wrong shows as
             * a fixture lit out of turn (or two at once). */
            case LZ_FX_WALK: {
                int on = n ? (int)(t * 2.0f) % n : 0;
                *s = (lz_look_t){ .pan = s->pan, .tilt = s->tilt };
                if (j - 1 == on) { s->dim = 255; s->r = s->g = s->b = s->w = 255; }
                break;
            }
            /* Colores: all of them red, green, blue, then white, in turn: a
             * fixture showing another colour has its channels in another
             * order than its type says. */
            case LZ_FX_RGBTEST: {
                int k = (int)(t * 2.0f) % 4;
                *s = (lz_look_t){ .dim = 255, .pan = s->pan, .tilt = s->tilt };
                if (k == 0) s->r = 255;
                else if (k == 1) s->g = 255;
                else if (k == 2) s->b = 255;
                else s->r = s->g = s->b = s->w = 255;
                break;
            }
            /* Respirar: the intensity up and down slowly, in the look's
             * colour (white when it has none): steps or flicker at the low
             * end are the fixture's dimming curve. */
            case LZ_FX_BREATHE:
                if (!s->r && !s->g && !s->b && !s->w) s->r = s->g = s->b = 255;
                s->dim = (uint8_t)(255.0f * (0.5f - 0.5f * cosf(6.2831853f * t * 0.25f)));
                s->strobe = 0;
                break;
            default: break;
            }
        }
    }
    /* the universe */
    memset(LZ.uni, 0, sizeof LZ.uni);
    unsigned m = LZ.blackout ? 0 : LZ.master;
    for (int i = 0; i < LZ.nfix; i++) {
        const lz_fix_t *f = &LZ.fix[i];
        const lz_type_t *t = &LZ.types[f->type];
        const lz_look_t *s = &LZ.shown[i];
        unsigned dim = (unsigned)s->dim * m / 255;
        for (int c = 0; c < t->n; c++) {
            int ch = f->addr + c;
            if (ch < 1 || ch > 512) break;
            unsigned v = 0;
            switch (t->attr[c]) {
            case LZ_A_DIM: v = dim; break;
            case LZ_A_R: v = s->r; break;
            case LZ_A_G: v = s->g; break;
            case LZ_A_B: v = s->b; break;
            case LZ_A_W: v = s->w; break;
            case LZ_A_AMBER: v = s->amber; break;
            case LZ_A_UV: v = s->uv; break;
            case LZ_A_STROBE: v = s->strobe; break;
            case LZ_A_PAN: v = s->pan >> 8; break;
            case LZ_A_PAN_F: v = s->pan & 0xFF; break;
            case LZ_A_TILT: v = s->tilt >> 8; break;
            case LZ_A_TILT_F: v = s->tilt & 0xFF; break;
            default: v = t->def[c]; break;
            }
            /* a type with no dimmer: the colours carry the intensity */
            if (!t->has_dim && t->attr[c] >= LZ_A_R && t->attr[c] <= LZ_A_UV) v = v * dim / 255;
            LZ.uni[ch] = (uint8_t)v;
        }
    }
}

lz_look_t lz_shown_color(int i, uint8_t *r, uint8_t *g, uint8_t *b)
{
    lz_look_t s = LZ.shown[i];
    unsigned m = LZ.blackout ? 0 : LZ.master;
    unsigned dim = (unsigned)s.dim * m / 255;
    const lz_type_t *t = &LZ.types[LZ.fix[i].type];
    unsigned R = s.r, G = s.g, B = s.b;
    if (!t->has_color) R = G = B = 255;                 /* a dimmer: white */
    R += s.w + s.amber; G += s.w + s.amber * 3 / 4; B += s.w + s.uv / 2;
    *r = (uint8_t)((R > 255 ? 255 : R) * dim / 255);
    *g = (uint8_t)((G > 255 ? 255 : G) * dim / 255);
    *b = (uint8_t)((B > 255 ? 255 : B) * dim / 255);
    return s;
}

/* Five scenes for whatever is patched: three plain colours, a rainbow
 * across the fixtures and two colours taking turns. A first show gets them,
 * and Escenas can add them again. */
int lz_scenes_demo(void)
{
    static const char *const NAME[5] = { N_("Rojo"), N_("Verde"), N_("Azul"), N_("Arcoíris"), N_("Pares e impares") };
    int added = 0;
    for (int k = 0; k < 5 && LZ.scenes && LZ.nscenes < LZ_SCENES; k++) {
        lz_scene_t *sc = &LZ.scenes[LZ.nscenes];
        memset(sc, 0, sizeof *sc);
        snprintf(sc->name, sizeof sc->name, "%.23s", aos_tr(NAME[k]));
        sc->fade_ms = 1000;
        for (int i = 0; i < LZ_FIX; i++) {
            lz_look_t *l = &sc->look[i];
            look_default(l);
            l->r = l->g = l->b = 0;
            l->dim = 255;
            if (k == 0) l->r = 255;
            else if (k == 1) l->g = 255;
            else if (k == 2) l->b = 255;
            else if (k == 3) hue(LZ.nfix ? (float)i / (float)LZ.nfix : 0, &l->r, &l->g, &l->b);
            else if (i % 2) { l->g = 200; l->b = 255; }
            else { l->r = 255; l->b = 200; }
        }
        LZ.nscenes++;
        added++;
    }
    LZ.changed++;
    lz_scenes_save();
    return added;
}
