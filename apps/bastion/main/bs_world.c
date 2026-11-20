/*
 * BASTION - a world's definition (see bs_world.h)
 */
#include "bs_world.h"

#include "bs_data.h"
#include "bs_pak.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *word(char **s)
{
    char *p = *s;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    char *a = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *s = p;
    return a;
}

static float fnum(char **s)
{
    char *w = word(s);
    return w ? strtof(w, NULL) : 0.0f;
}

static int inum(char **s)
{
    char *w = word(s);
    return w ? atoi(w) : 0;
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static bool path_read(bs_path_t *p, char **s)
{
    int n = inum(s);
    if (n < 2 || n > 2000) return false;
    p->x = (float *)bs_malloc(sizeof(float) * (size_t)n * 3);
    if (!p->x) return false;
    p->y = p->x + n;
    p->s = p->y + n;
    p->n = n;
    for (int i = 0; i < n; i++) {
        p->x[i] = fnum(s);
        p->y[i] = fnum(s);
        p->s[i] = i ? p->s[i - 1] + hypotf(p->x[i] - p->x[i - 1], p->y[i] - p->y[i - 1]) : 0.0f;
    }
    p->len = p->s[n - 1];
    return true;
}

static int lang_of(const char *l)
{
    return !strncmp(l, "en", 2) ? 1 : !strncmp(l, "de", 2) ? 2 : 0;
}

bool bs_world_parse(bs_world_t *w, char *text, bool head_only)
{
    memset(w, 0, sizeof *w);
    w->boss = AL_QUEEN;
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *s = line;
        char *k = word(&s);
        if (k) {
            if (!strcmp(k, "id")) {
                char *v = word(&s);
                if (v) copy(w->id, sizeof w->id, v);
            } else if (!strcmp(k, "order")) {
                w->order = inum(&s);
            } else if (!strcmp(k, "name") || !strcmp(k, "desc")) {
                while (*s == ' ') s++;
                char *bar = strchr(s, '|');
                if (bar) {
                    int l = lang_of(s);
                    if (k[0] == 'n') copy(w->name[l], sizeof w->name[l], bar + 1);
                    else copy(w->desc[l], sizeof w->desc[l], bar + 1);
                }
            } else if (!strcmp(k, "accent")) {
                char *v = word(&s);
                w->accent = v ? (uint32_t)strtol(v, NULL, 16) : 0x40C8FF;
            } else if (!strcmp(k, "sky")) {
                char *v = word(&s);
                w->sky = v ? (uint32_t)strtol(v, NULL, 16) : 0;
            } else if (!strcmp(k, "hue")) {
                w->hue = inum(&s);
            } else if (!strcmp(k, "music")) {
                w->music = inum(&s);
            } else if (!strcmp(k, "boss")) {
                char *v = word(&s);
                int b = v ? bs_alien_by_id(v) : -1;
                if (b >= 0) w->boss = b;
            } else if (!strcmp(k, "fort")) {
                w->fort_x = fnum(&s);
                w->fort_y = fnum(&s);
            } else if (!strcmp(k, "sector")) {
                int sec = inum(&s);
                if (sec >= 0 && sec < BS_SECTORS) {
                    w->nwaves[sec] = inum(&s);
                    if (w->nwaves[sec] > BS_WAVES) w->nwaves[sec] = BS_WAVES;
                    w->credits[sec] = inum(&s);
                }
            } else if (head_only) {
                /* the rest is the battle's */
            } else if (!strcmp(k, "path") || !strcmp(k, "air")) {
                int i = inum(&s);
                if (i >= 0 && i < BS_PATHS) {
                    if (!path_read(k[0] == 'p' ? &w->path[i] : &w->air[i], &s)) return false;
                    if (k[0] == 'p' && i + 1 > w->npath) w->npath = i + 1;
                }
            } else if (!strcmp(k, "pad")) {
                if (w->npad < BS_PADS) {
                    w->pad_x[w->npad] = fnum(&s);
                    w->pad_y[w->npad] = fnum(&s);
                    w->npad++;
                }
            } else if (!strcmp(k, "wave")) {
                int sec = inum(&s), wv = inum(&s);
                if (sec >= 0 && sec < BS_SECTORS && wv >= 0 && wv < BS_WAVES) {
                    w->wave[sec][wv].first = (uint16_t)w->ngroup;
                    w->wave[sec][wv].n = 0;
                }
                /* its groups follow; remembered through 'first' */
                w->cap = (sec << 8) | wv;
            } else if (!strcmp(k, "g")) {
                int sec = (w->cap >> 8) & 255, wv = w->cap & 255;
                char *id = word(&s);
                int al = id ? bs_alien_by_id(id) : -1;
                if (al >= 0 && sec < BS_SECTORS && wv < BS_WAVES) {
                    if ((w->ngroup & 63) == 0) {
                        bs_group_t *ng = (bs_group_t *)bs_malloc(sizeof(bs_group_t) * (size_t)(w->ngroup + 64));
                        if (!ng) return false;
                        if (w->group) memcpy(ng, w->group, sizeof(bs_group_t) * (size_t)w->ngroup);
                        bs_free(w->group);
                        w->group = ng;
                    }
                    bs_group_t *g = &w->group[w->ngroup++];
                    g->alien = (uint8_t)al;
                    g->count = (uint16_t)inum(&s);
                    g->gap = fnum(&s);
                    g->delay = fnum(&s);
                    int p = inum(&s);
                    g->path = (uint8_t)(p < 0 ? 0 : p);
                    g->hp_pct = (uint16_t)inum(&s);
                    w->wave[sec][wv].n++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!w->id[0]) return false;
    if (head_only) return true;
    if (!w->npath || !w->npad) return false;
    for (int i = 0; i < w->ngroup; i++) {
        if (w->group[i].path >= w->npath) w->group[i].path = 0;
    }
    /* a world without its fliers' ways gets straight lines to the fort */
    for (int i = 0; i < w->npath; i++) {
        if (w->air[i].n) continue;
        bs_path_t *a = &w->air[i];
        a->x = (float *)bs_malloc(sizeof(float) * 6);
        if (!a->x) return false;
        a->y = a->x + 2;
        a->s = a->y + 2;
        a->n = 2;
        a->x[0] = w->path[i].x[0];
        a->y[0] = w->path[i].y[0];
        a->x[1] = w->fort_x;
        a->y[1] = w->fort_y;
        a->s[0] = 0;
        a->s[1] = a->len = hypotf(a->x[1] - a->x[0], a->y[1] - a->y[0]);
    }
    return true;
}

void bs_world_free(bs_world_t *w)
{
    for (int i = 0; i < BS_PATHS; i++) {
        bs_free(w->path[i].x);
        bs_free(w->air[i].x);
    }
    bs_free(w->group);
    memset(w, 0, sizeof *w);
}

void bs_path_at(const bs_path_t *p, float s, float *x, float *y, float *dx, float *dy)
{
    if (p->n < 2) {
        *x = *y = 0;
        if (dx) *dx = 0;
        if (dy) *dy = -1;
        return;
    }
    if (s < 0) s = 0;
    if (s > p->len) s = p->len;
    /* the points are evenly spaced: a guess, then a step or two */
    int i = (int)(s / p->len * (float)(p->n - 1));
    if (i > p->n - 2) i = p->n - 2;
    while (i > 0 && p->s[i] > s) i--;
    while (i < p->n - 2 && p->s[i + 1] < s) i++;
    float seg = p->s[i + 1] - p->s[i];
    float t = seg > 1e-5f ? (s - p->s[i]) / seg : 0.0f;
    *x = p->x[i] + (p->x[i + 1] - p->x[i]) * t;
    *y = p->y[i] + (p->y[i + 1] - p->y[i]) * t;
    if (dx && dy) {
        /* the way it runs, eased over the neighbours so a walker turns
         * through a bend instead of snapping */
        int a = i > 0 ? i - 1 : i, b = i + 2 < p->n ? i + 2 : p->n - 1;
        float vx = p->x[b] - p->x[a], vy = p->y[b] - p->y[a];
        float l = hypotf(vx, vy);
        *dx = l > 1e-5f ? vx / l : 0.0f;
        *dy = l > 1e-5f ? vy / l : -1.0f;
    }
}

uint32_t bs_world_aliens(const bs_world_t *w, int sector)
{
    uint32_t m = 0;
    if (sector < 0 || sector >= BS_SECTORS) return 0;
    for (int wv = 0; wv < w->nwaves[sector]; wv++) {
        const bs_wave_t *wa = &w->wave[sector][wv];
        for (int i = 0; i < wa->n; i++) m |= 1u << w->group[wa->first + i].alien;
    }
    /* what they bring with them */
    if (m & (1u << AL_SPLIT)) m |= 1u << AL_MINI;
    if (m & (1u << AL_CARRIER)) m |= 1u << AL_SWARM;
    if (m & (1u << AL_QUEEN)) m |= 1u << AL_GRUB;
    return m;
}
