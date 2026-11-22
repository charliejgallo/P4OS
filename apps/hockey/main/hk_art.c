/*
 * HOCKEY - the art (see hk_art.h)
 */
#include "hk_art.h"

#include "aos_hal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *keep(hk_art_t *a, uint8_t *b)
{
    if (b && a->nblk < (int)(sizeof a->blk / sizeof a->blk[0])) a->blk[a->nblk++] = b;
    return b;
}

static bool font(hk_art_t *a, hk_font_t *f, const char *name)
{
    const hk_entry_t *e;
    uint8_t *d = keep(a, (uint8_t *)hk_pak_get(&a->pak, name, &e));
    return d && hk_font_bind(f, d, e->rawlen);
}

bool hk_art_open(hk_art_t *a, const char *apps)
{
    memset(a, 0, sizeof *a);
    a->cur = -1;
    char path[200];
    snprintf(path, sizeof path, "%.150s/hockey_p4.pak", apps);
    if (!hk_pak_open(&a->pak, path)) {
        aos_hal_log("hockey", "no %s", path);
        return false;
    }
    bool ok = font(a, &a->f_small, "f_small");
    ok = font(a, &a->f_score, "f_score") && ok;
    ok = font(a, &a->f_big, "f_big") && ok;
    ok = font(a, &a->f_clock, "f_clock") && ok;
    a->ok = ok && !a->pak.fails;
    if (!a->ok) aos_hal_log("hockey", "the common pack is incomplete (%d failed)", a->pak.fails);
    hk_tables_scan(a, apps);
    return a->ok && a->ntb > 0;
}

void hk_art_close(hk_art_t *a)
{
    hk_art_table_free(a);
    for (int i = 0; i < a->nblk; i++) hk_free(a->blk[i]);
    hk_pak_close(&a->pak);
    memset(a, 0, sizeof *a);
    a->cur = -1;
}

/* the next line of a text, cut in place (strtok is not in the apps' table) */
static char *next_line(char **p)
{
    char *s = *p;
    if (!s || !*s) return NULL;
    char *e = strchr(s, '\n');
    if (e) {
        *e = 0;
        *p = e + 1;
    } else {
        *p = s + strlen(s);
    }
    return s;
}

/* ---- the tables ---------------------------------------------------------------------------------- */

static void def_read(hk_table_t *t, char *txt)
{
    char *cur = txt;
    for (char *line; (line = next_line(&cur));) {
        char *eq = strstr(line, " = ");
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 3;
        if (!strcmp(line, "id")) snprintf(t->id, sizeof t->id, "%.15s", v);
        else if (!strcmp(line, "nombre")) snprintf(t->name[0], sizeof t->name[0], "%.39s", v);
        else if (!strcmp(line, "name_en")) snprintf(t->name[1], sizeof t->name[1], "%.39s", v);
        else if (!strcmp(line, "name_de")) snprintf(t->name[2], sizeof t->name[2], "%.39s", v);
        else if (!strcmp(line, "acento")) t->accent = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(line, "lado0")) t->side[0] = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(line, "lado1")) t->side[1] = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(line, "disco")) t->puck = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(line, "brillo")) t->glow = v[0] == '1';
    }
}

static void scan_pack(hk_art_t *a, const char *path)
{
    hk_pak_t p;
    if (!hk_pak_open(&p, path)) return;
    for (int i = 0; i < p.n && a->ntb < HK_TABLES_MAX; i++) {
        size_t n = strlen(p.e[i].name);
        if (n < 5 || strcmp(p.e[i].name + n - 4, ".def")) continue;
        char *txt = (char *)hk_pak_load(&p, &p.e[i]);
        if (!txt) continue;
        hk_table_t *t = &a->tb[a->ntb];
        memset(t, 0, sizeof *t);
        def_read(t, txt);
        hk_free(txt);
        if (!t->id[0] || hk_table_find(a, t->id) >= 0) continue;
        snprintf(t->path, sizeof t->path, "%.159s", path);
        a->ntb++;
    }
    hk_pak_close(&p);
}

/* the order of tools/pack_p4.py, whatever order the card lists them in */
static const char *const ORDER[] = { "bar", "neon", "hielo" };

static int rank(const char *id)
{
    for (int i = 0; i < (int)(sizeof ORDER / sizeof ORDER[0]); i++) {
        if (!strcmp(ORDER[i], id)) return i;
    }
    return 100;
}

void hk_tables_scan(hk_art_t *a, const char *apps)
{
    a->ntb = 0;
    char path[200];
    snprintf(path, sizeof path, "%.150s/hockey_p4.pak", apps);
    scan_pack(a, path);
    DIR *d = opendir(apps);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *n = de->d_name;
            size_t l = strlen(n);
            if (n[0] == '.' || strncmp(n, "hockey_", 7) || l < 11 || strcmp(n + l - 7, "_p4.pak") ||
                !strcmp(n, "hockey_p4.pak"))
                continue;
            snprintf(path, sizeof path, "%.150s/%.40s", apps, n);
            scan_pack(a, path);
        }
        closedir(d);
    }
    for (int i = 1; i < a->ntb; i++) {
        for (int j = i; j > 0 && rank(a->tb[j].id) < rank(a->tb[j - 1].id); j--) {
            hk_table_t t = a->tb[j];
            a->tb[j] = a->tb[j - 1];
            a->tb[j - 1] = t;
        }
    }
}

int hk_table_find(const hk_art_t *a, const char *id)
{
    for (int i = 0; i < a->ntb; i++) {
        if (!strcmp(a->tb[i].id, id)) return i;
    }
    return -1;
}

void hk_art_table_free(hk_art_t *a)
{
    hk_free(a->bg);
    hk_free(a->d_near);
    hk_free(a->d_far);
    hk_free(a->d_puck);
    a->bg = NULL;
    a->d_near = a->d_far = a->d_puck = NULL;
    memset(&a->near, 0, sizeof a->near);
    memset(&a->far, 0, sizeof a->far);
    memset(&a->puck, 0, sizeof a->puck);
    for (int s = 0; s < 2; s++) {
        for (int k = 0; k < 3; k++) {
            hk_free(a->d_mallet[s][k]);
            a->d_mallet[s][k] = NULL;
            memset(&a->mallet[s][k], 0, sizeof a->mallet[s][k]);
        }
    }
    a->cur = -1;
}

static bool part_sheet(hk_pak_t *p, const char *id, const char *what, hk_sheet_t *s, uint8_t **d)
{
    char name[HK_NAME_LEN];
    snprintf(name, sizeof name, "%.15s.%.6s", id, what);
    const hk_entry_t *e;
    *d = (uint8_t *)hk_pak_get(p, name, &e);
    return *d && hk_sheet_bind(s, *d, e->rawlen);
}

bool hk_art_table(hk_art_t *a, int tb)
{
    if (tb < 0 || tb >= a->ntb) return false;
    if (a->cur == tb && a->bg) return true;
    hk_art_table_free(a);
    const hk_table_t *t = &a->tb[tb];
    hk_pak_t p;
    if (!hk_pak_open(&p, t->path)) return false;
    char name[HK_NAME_LEN];
    snprintf(name, sizeof name, "%.15s.bg", t->id);
    const hk_entry_t *e;
    uint8_t *jpg = (uint8_t *)hk_pak_get(&p, name, &e);
    bool ok = false;
    if (jpg) {
        a->bg = (uint16_t *)hk_malloc((size_t)HK_W * HK_H * 2);
        aos_jpeg_t *j = aos_hal_jpeg_open();
        int w = 0, h = 0, stride = 0;
        const uint16_t *px = j && a->bg ? aos_hal_jpeg_decode(j, jpg, e->rawlen, &w, &h, &stride) : NULL;
        if (px && w >= HK_W && h >= HK_H) {
            for (int y = 0; y < HK_H; y++) memcpy(a->bg + (size_t)y * HK_W, px + (size_t)y * stride, HK_W * 2);
            ok = true;
        } else {
            aos_hal_log("hockey", "cannot decode %s (%dx%d)", name, w, h);
        }
        if (j) aos_hal_jpeg_close(j);
        hk_free(jpg);
    }
    ok = ok && part_sheet(&p, t->id, "near", &a->near, &a->d_near);
    ok = ok && part_sheet(&p, t->id, "far", &a->far, &a->d_far);
    ok = ok && part_sheet(&p, t->id, "puck", &a->puck, &a->d_puck);
    static const char SZ[3] = { 's', 'm', 'l' };
    for (int s = 0; s < 2 && ok; s++) {
        for (int k = 0; k < 3 && ok; k++) {
            char what[8];
            snprintf(what, sizeof what, "m%d%c", s, SZ[k]);
            ok = part_sheet(&p, t->id, what, &a->mallet[s][k], &a->d_mallet[s][k]);
        }
    }
    if (p.fails) ok = false;
    hk_pak_close(&p);
    if (!ok) {
        hk_art_table_free(a);
        return false;
    }
    a->cur = tb;
    return true;
}
