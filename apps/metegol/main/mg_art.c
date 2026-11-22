/*
 * METEGOL - the art (see mg_art.h)
 */
#include "mg_art.h"

#include "aos_hal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *keep(mg_art_t *a, uint8_t *b)
{
    if (b && a->nblk < (int)(sizeof a->blk / sizeof a->blk[0])) a->blk[a->nblk++] = b;
    return b;
}

static bool sheet(mg_art_t *a, mg_sheet_t *s, const char *name)
{
    const mg_entry_t *e;
    uint8_t *d = keep(a, (uint8_t *)mg_pak_get(&a->pak, name, &e));
    return d && mg_sheet_bind(s, d, e->rawlen);
}

static bool font(mg_art_t *a, mg_font_t *f, const char *name)
{
    const mg_entry_t *e;
    uint8_t *d = keep(a, (uint8_t *)mg_pak_get(&a->pak, name, &e));
    return d && mg_font_bind(f, d, e->rawlen);
}

bool mg_art_open(mg_art_t *a, const char *apps)
{
    memset(a, 0, sizeof *a);
    a->cur_st = -1;
    char path[200];
    snprintf(path, sizeof path, "%.150s/metegol_p4.pak", apps);
    if (!mg_pak_open(&a->pak, path)) {
        aos_hal_log("metegol", "no %s", path);
        return false;
    }
    const mg_entry_t *e;
    a->figs = (uint8_t *)mg_pak_get(&a->pak, "figs", &e);
    a->figs_len = a->figs ? e->rawlen : 0;
    uint8_t *pr = (uint8_t *)mg_pak_get(&a->pak, "rod_prof", &e);
    if (pr && e->w <= 24) {
        a->rod_prof.w = e->w;
        for (int i = 0; i < e->w; i++) {
            a->rod_prof.px[i] = (uint16_t)(pr[i * 2] | (pr[i * 2 + 1] << 8));
            a->rod_prof.a[i] = pr[e->w * 2 + i];
        }
        a->rod_prof.id = 0xC0DE0001u;
        a->rod_prof_x = e->x;
    }
    mg_free(pr);
    bool ok = a->figs && a->rod_prof.w > 0;
    ok = sheet(a, &a->bumper, "rod_bumper") && ok;
    ok = sheet(a, &a->hnear, "rod_handle_near") && ok;
    ok = sheet(a, &a->hfar, "rod_handle_far") && ok;
    ok = sheet(a, &a->enear, "rod_end_near") && ok;
    ok = sheet(a, &a->efar, "rod_end_far") && ok;
    ok = sheet(a, &a->ball, "ball") && ok;
    ok = font(a, &a->f_small, "f_small") && ok;
    ok = font(a, &a->f_score, "f_score") && ok;
    ok = font(a, &a->f_big, "f_big") && ok;
    a->ok = ok && !a->pak.fails;
    if (!a->ok) aos_hal_log("metegol", "the common pack is incomplete (%d failed)", a->pak.fails);
    mg_stadiums_scan(a, apps);
    return a->ok;
}

void mg_art_close(mg_art_t *a)
{
    mg_art_stadium_free(a);
    mg_art_teams_free(a);
    for (int i = 0; i < a->nblk; i++) mg_free(a->blk[i]);
    mg_free(a->figs);
    mg_pak_close(&a->pak);
    memset(a, 0, sizeof *a);
    a->cur_st = -1;
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

/* ---- the stadiums ------------------------------------------------------------------------------------ */

static void def_read(mg_stadium_t *s, char *txt)
{
    char *cur = txt;
    for (char *line; (line = next_line(&cur));) {
        char *eq = strstr(line, " = ");
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 3;
        if (!strcmp(line, "id")) snprintf(s->id, sizeof s->id, "%.23s", v);
        else if (!strcmp(line, "nombre")) snprintf(s->name[0], sizeof s->name[0], "%.47s", v);
        else if (!strcmp(line, "name_en")) snprintf(s->name[1], sizeof s->name[1], "%.47s", v);
        else if (!strcmp(line, "name_de")) snprintf(s->name[2], sizeof s->name[2], "%.47s", v);
        else if (!strcmp(line, "liga")) s->league = v[0] == 'c' ? MG_ST_CLUBS : v[0] == 's' ? MG_ST_NATIONS : MG_ST_BOTH;
        else if (!strcmp(line, "acento")) s->accent = (uint32_t)strtoul(v, NULL, 16);
    }
}

static void scan_pack(mg_art_t *a, const char *path)
{
    mg_pak_t p;
    if (!mg_pak_open(&p, path)) return;
    for (int i = 0; i < p.n && a->nst < MG_STADIUMS_MAX; i++) {
        size_t n = strlen(p.e[i].name);
        if (n < 5 || strcmp(p.e[i].name + n - 4, ".def")) continue;
        char *txt = (char *)mg_pak_load(&p, &p.e[i]);
        if (!txt) continue;
        mg_stadium_t *s = &a->st[a->nst];
        memset(s, 0, sizeof *s);
        def_read(s, txt);
        mg_free(txt);
        if (!s->id[0] || mg_stadium_find(a, s->id) >= 0) continue;
        snprintf(s->path, sizeof s->path, "%.159s", path);
        a->nst++;
    }
    mg_pak_close(&p);
}

/* the order of tools/pack_p4.py, whatever order the card lists them in */
static const char *const ORDER[] = { "laboca", "nunez", "rio", "madrid", "munich", "londres", "mexico", "tokio",
                                     "avella_rojo", "avella_celeste" };

static int rank(const char *id)
{
    for (int i = 0; i < (int)(sizeof ORDER / sizeof ORDER[0]); i++) {
        if (!strcmp(ORDER[i], id)) return i;
    }
    return 100;
}

void mg_stadiums_scan(mg_art_t *a, const char *apps)
{
    a->nst = 0;
    char path[200];
    snprintf(path, sizeof path, "%.150s/metegol_p4.pak", apps);
    scan_pack(a, path);
    DIR *d = opendir(apps);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            const char *n = de->d_name;
            size_t l = strlen(n);
            if (n[0] == '.' || strncmp(n, "metegol_", 8) || l < 12 || strcmp(n + l - 7, "_p4.pak") ||
                !strcmp(n, "metegol_p4.pak"))
                continue;
            snprintf(path, sizeof path, "%.150s/%.40s", apps, n);
            scan_pack(a, path);
        }
        closedir(d);
    }
    for (int i = 1; i < a->nst; i++) {
        for (int j = i; j > 0 && rank(a->st[j].id) < rank(a->st[j - 1].id); j--) {
            mg_stadium_t t = a->st[j];
            a->st[j] = a->st[j - 1];
            a->st[j - 1] = t;
        }
    }
}

int mg_stadium_find(const mg_art_t *a, const char *id)
{
    for (int i = 0; i < a->nst; i++) {
        if (!strcmp(a->st[i].id, id)) return i;
    }
    return -1;
}

void mg_art_stadium_free(mg_art_t *a)
{
    mg_free(a->bg);
    mg_free(a->near_d);
    mg_free(a->far_d);
    mg_free(a->crowd_d);
    a->bg = NULL;
    a->near_d = a->far_d = a->crowd_d = NULL;
    memset(&a->near, 0, sizeof a->near);
    memset(&a->far, 0, sizeof a->far);
    memset(&a->crowd, 0, sizeof a->crowd);
    a->crowd_n = 0;
    a->crowd_strips = 0;
    a->nflash = 0;
    a->cur_st = -1;
}

static bool part_sheet(mg_pak_t *p, const char *id, const char *what, mg_sheet_t *s, uint8_t **d)
{
    char name[MG_NAME_LEN];
    snprintf(name, sizeof name, "%.16s.%.6s", id, what);
    const mg_entry_t *e;
    *d = (uint8_t *)mg_pak_get(p, name, &e);
    return *d && mg_sheet_bind(s, *d, e->rawlen);
}

bool mg_art_stadium(mg_art_t *a, int st)
{
    if (st < 0 || st >= a->nst) return false;
    if (a->cur_st == st && a->bg) return true;
    mg_art_stadium_free(a);
    const mg_stadium_t *s = &a->st[st];
    mg_pak_t p;
    if (!mg_pak_open(&p, s->path)) return false;
    char name[MG_NAME_LEN];
    snprintf(name, sizeof name, "%.19s.bg", s->id);
    const mg_entry_t *e;
    uint8_t *jpg = (uint8_t *)mg_pak_get(&p, name, &e);
    bool ok = false;
    if (jpg) {
        a->bg = (uint16_t *)mg_malloc((size_t)MG_W * MG_H * 2);
        aos_jpeg_t *j = aos_hal_jpeg_open();
        int w = 0, h = 0, stride = 0;
        const uint16_t *px = j && a->bg ? aos_hal_jpeg_decode(j, jpg, e->rawlen, &w, &h, &stride) : NULL;
        if (px && w >= MG_W && h >= MG_H) {
            for (int y = 0; y < MG_H; y++) memcpy(a->bg + (size_t)y * MG_W, px + (size_t)y * stride, MG_W * 2);
            ok = true;
        } else {
            aos_hal_log("metegol", "cannot decode %s (%dx%d)", name, w, h);
        }
        if (j) aos_hal_jpeg_close(j);
        mg_free(jpg);
    }
    ok = ok && part_sheet(&p, s->id, "near", &a->near, &a->near_d);
    ok = ok && part_sheet(&p, s->id, "far", &a->far, &a->far_d);
    if (ok && part_sheet(&p, s->id, "crowd", &a->crowd, &a->crowd_d)) {
        /* the strips are in the entry's x (pack_p4.py), the frames a strip in its cols */
        snprintf(name, sizeof name, "%.16s.crowd", s->id);
        const mg_entry_t *ce = mg_pak_find(&p, name);
        a->crowd_strips = ce && ce->x > 0 ? ce->x : 1;
        a->crowd_n = a->crowd.nframes / a->crowd_strips;
    }
    /* the crowd's flashes */
    snprintf(name, sizeof name, "%.19s.def", s->id);
    char *txt = (char *)mg_pak_get(&p, name, NULL);
    if (txt) {
        char *cur = txt;
    for (char *line; (line = next_line(&cur));) {
            int x, y;
            if (sscanf(line, "flash = %d,%d", &x, &y) == 2 && a->nflash < MG_FLASH_MAX) {
                a->flash[a->nflash][0] = (int16_t)x;
                a->flash[a->nflash][1] = (int16_t)y;
                a->nflash++;
            }
        }
        mg_free(txt);
    }
    if (p.fails) ok = false;
    mg_pak_close(&p);
    if (!ok) {
        mg_art_stadium_free(a);
        return false;
    }
    a->cur_st = st;
    return true;
}

/* ---- the teams ---------------------------------------------------------------------------------------- */

void mg_art_teams_free(mg_art_t *a)
{
    for (int t = 0; t < 2; t++) {
        for (int m = 0; m < 2; m++) {
            mg_free(a->fig_d[t][m]);
            a->fig_d[t][m] = NULL;
            memset(&a->fig[t][m], 0, sizeof a->fig[t][m]);
        }
        mg_free(a->badge_d[t]);
        a->badge_d[t] = NULL;
        memset(&a->badge[t], 0, sizeof a->badge[t]);
    }
}

#define BADGE_W 36
#define BADGE_H 24

bool mg_art_teams(mg_art_t *a, const mg_team_t *t0, const mg_team_t *t1)
{
    mg_art_teams_free(a);
    const mg_team_t *t[2] = { t0, t1 };
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        for (int m = 0; m < 2 && ok; m++) {
            size_t len = 0;
            a->fig_d[i][m] = mg_kit_paint(a->figs, a->figs_len, t[i], m == 1, &len);
            ok = a->fig_d[i][m] && mg_sheet_bind(&a->fig[i][m], a->fig_d[i][m], len);
        }
        if (!ok) break;
        /* the badge of the scoreboard: the flag, or a patch of the shirt, framed */
        uint16_t px[(BADGE_W + 2) * (BADGE_H + 2)];
        uint8_t al[(BADGE_W + 2) * (BADGE_H + 2)];
        uint16_t in[BADGE_W * BADGE_H];
        mg_kit_badge(t[i], in, BADGE_W, BADGE_H);
        for (int y = 0; y < BADGE_H + 2; y++) {
            for (int x = 0; x < BADGE_W + 2; x++) {
                bool edge = x == 0 || y == 0 || x == BADGE_W + 1 || y == BADGE_H + 1;
                px[y * (BADGE_W + 2) + x] = edge ? 0x2104 : in[(y - 1) * BADGE_W + x - 1];
                al[y * (BADGE_W + 2) + x] = 255;
            }
        }
        mg_rgba_frame_t fr = { px, al, BADGE_W + 2, BADGE_H + 2, 0, 0 };
        size_t len = 0;
        a->badge_d[i] = mg_sheet_encode(&fr, 1, 1, &len);
        ok = a->badge_d[i] && mg_sheet_bind(&a->badge[i], a->badge_d[i], len);
    }
    if (!ok) mg_art_teams_free(a);
    return ok;
}
