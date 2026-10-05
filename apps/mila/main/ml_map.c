/*
 * MILA - the world map (see ml_map.h)
 *
 * Strip coordinates: pixels from the top of the whole strip. Panels are
 * stacked edge to edge, top to bottom: map_soon, the last world ... the
 * first world, map_home (each panel's picture carries the black gap and the
 * paw prints that join it to the next).
 *
 * P4OS: the panels are 720 px wide (map.py at ML_RES 720/368): the whole
 * width upright, a column in the middle lying down (MX() is where it
 * starts). The stones, the ring and Mila's marker are drawn at the same
 * scale, and the strip only redraws what changed: all of it while it
 * scrolls, the current stone's ring and marker while it stands.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "ml_app.h"
#include "ml_audio.h"
#include "ml_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAP         0           /* the panels carry their own gap and paw prints */
#define STRIP_W     720         /* the panels' width                        */
#define MX()        ((ML_W - STRIP_W) / 2)
#define MK          (720.0f / 368.0f)   /* the map's pixels per watch pixel */
#define WIDE()      (ML_W > ML_H)
#define HOME_X      (WIDE() ? 100 : MX() + 70)
#define HOME_Y      (ML_H - (WIDE() ? 100 : 74))
#define HOME_R      40
/* lying down, the worlds in a column right of the strip: a tap goes there */
#define LIST_X      (MX() + STRIP_W + 24)
#define LIST_Y      60
#define LIST_ROW    118
#define MAXP        (ML_MAX_WORLDS + 2)
#define MAXN        ML_MAX_LEVELS

typedef struct {
    ml_anim_t pic;
    int       world;            /* -1 home, -2 soon                        */
    int       y;                /* strip y of its top                      */
    int       h;
    int       nn;
    int16_t   node[MAXN][2];    /* stones, panel px                        */
    int16_t   house[4];         /* home: the house's tap box               */
    bool      open;
} panel_t;

typedef struct {
    panel_t   p[MAXP];
    int       np;
    int       strip_h;
    ml_anim_t stone, stone_done, stone_locked, ring, marker;
    ml_anim_t emblem[ML_MAX_WORLDS];    /* lying down, the list's            */
    float     go_to;                /* a scroll to glide to, -1 none         */
    float     scroll, vel;      /* strip y at the screen's top             */
    float     t;
    int       drawn_sc;         /* the scroll of the last frame, -1 none   */
    int       drawn_coins, drawn_stars;
    int       cur_w, cur_l;     /* where Mila stands (the gamepad moves her) */
    int       drawn_w, drawn_l; /* where she stood in the last frame        */
    /* the finger (LVGL thread writes) */
    volatile bool   held;
    volatile float  drag_to;
    volatile bool   tap;
    volatile int    tap_x, tap_y;
    float     last_y;
    uint32_t  last_ms;
    float     fling;
    int       press_x, press_y;
    bool      pressed, dragged;
} map_t;

static map_t *M(app_t *a) { return (map_t *)a->map; }

static void load_nodes(panel_t *p, const char *panel)
{
    char nm[48];
    snprintf(nm, sizeof nm, "%s_nodes", panel);
    uint32_t len = 0;
    char *b = (char *)ml_art_blob(nm, &len);
    if (!b) return;
    for (char *line = b; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        if (!strncmp(line, "nodes ", 6)) {
            char *q = line + 6;
            int x, y, used;
            while (p->nn < MAXN && sscanf(q, "%d,%d%n", &x, &y, &used) == 2) {
                p->node[p->nn][0] = (int16_t)x;
                p->node[p->nn][1] = (int16_t)y;
                p->nn++;
                q += used;
                while (*q == ' ') q++;
            }
        } else if (!strncmp(line, "house ", 6)) {
            int v[4];
            if (sscanf(line + 6, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4)
                for (int i = 0; i < 4; i++) p->house[i] = (int16_t)v[i];
        }
        line = eol ? eol + 1 : NULL;
    }
    free(b);
}

/* the level Mila stands on: the last played, else the first not solved */
static void pick_current(app_t *a, map_t *m)
{
    m->cur_w = 0;
    m->cur_l = 0;
    char id[24];
    int lv = 0;
    if (sscanf(a->prog.last, "%23s %d", id, &lv) == 2) {
        int w = ml_worlds_find(&a->worlds, id);
        if (w >= 0 && lv >= 0 && lv < a->worlds.w[w].nlevels) {
            m->cur_w = w;
            m->cur_l = lv;
            /* solved it: the next one, if open */
            if (a->prog.stars[w][lv] && lv + 1 < a->worlds.w[w].nlevels && mla_level_open(a, w, lv + 1))
                m->cur_l = lv + 1;
            return;
        }
    }
    for (int w = 0; w < a->worlds.nworlds; w++)
        for (int l = 0; l < a->worlds.w[w].nlevels; l++)
            if (!a->prog.stars[w][l] && mla_level_open(a, w, l)) {
                m->cur_w = w;
                m->cur_l = l;
                return;
            }
}

bool mlm_open(app_t *a)
{
    mlm_close(a);
    map_t *m = (map_t *)ml_calloc(1, sizeof(map_t));
    if (!m) return false;
    /* top to bottom: soon, worlds last..first, home */
    int order[MAXP], n = 0;
    order[n++] = -2;
    for (int w = a->worlds.nworlds - 1; w >= 0 && n < MAXP - 1; w--) order[n++] = w;
    order[n++] = -1;
    int y = 0;
    for (int i = 0; i < n; i++) {
        panel_t *p = &m->p[m->np];
        p->world = order[i];
        const char *name = p->world == -1 ? "map_home" : p->world == -2 ? "map_soon" : a->worlds.w[p->world].panel;
        if (!ml_art_load(name, &p->pic)) {
            if (p->world == -2) continue;           /* no "soon" yet: fine */
        }
        p->h = p->pic.n ? p->pic.f[0].h : 300;
        p->y = y;
        p->open = p->world < 0 || mla_world_open(a, p->world);
        load_nodes(p, name);
        y += p->h + GAP;
        m->np++;
        ml_yield();
    }
    m->strip_h = y - GAP;
    ml_art_load("map_stone", &m->stone);
    ml_art_load("map_stone_done", &m->stone_done);
    ml_art_load("map_stone_locked", &m->stone_locked);
    ml_art_load("map_stone_ring", &m->ring);
    ml_art_load("map_mila", &m->marker);
    for (int w = 0; w < a->worlds.nworlds && w < ML_MAX_WORLDS; w++) {
        char nm[48];
        snprintf(nm, sizeof nm, "ui_emblem_%s", a->worlds.w[w].id);
        if (ml_art_has(nm)) ml_art_load(nm, &m->emblem[w]);
    }
    pick_current(a, m);
    /* start with Mila's stone a little below the middle */
    m->scroll = (float)(m->strip_h - ML_H);
    for (int i = 0; i < m->np; i++) {
        panel_t *p = &m->p[i];
        if (p->world == m->cur_w && m->cur_l < p->nn) m->scroll = (float)(p->y + p->node[m->cur_l][1] - ML_H * 0.58f);
    }
    if (m->scroll < 0) m->scroll = 0;
    if (m->scroll > m->strip_h - ML_H) m->scroll = (float)(m->strip_h - ML_H);
    m->drag_to = -1;
    m->go_to = -1;
    m->drawn_sc = -1;
    a->map = m;
    return true;
}

void mlm_close(app_t *a)
{
    map_t *m = M(a);
    if (!m) return;
    a->map = NULL;
    for (int i = 0; i < m->np; i++) ml_anim_free(&m->p[i].pic);
    ml_anim_free(&m->stone);
    ml_anim_free(&m->stone_done);
    ml_anim_free(&m->stone_locked);
    ml_anim_free(&m->ring);
    ml_anim_free(&m->marker);
    for (int w = 0; w < ML_MAX_WORLDS; w++) ml_anim_free(&m->emblem[w]);
    free(m);
}

void mlm_step(app_t *a, float dt)
{
    map_t *m = M(a);
    if (!m) return;
    m->t += dt;
    if (m->held) {
        float to = m->drag_to;
        if (to >= 0) m->scroll = to;
        m->vel = m->fling;
        m->go_to = -1;
    } else if (m->go_to >= 0) {
        float d = m->go_to - m->scroll;
        m->scroll += d * (1.0f - expf(-dt * 8.0f));
        if (fabsf(d) < 1.0f) {
            m->scroll = m->go_to;
            m->go_to = -1;
        }
        m->vel = 0;
    } else {
        m->scroll += m->vel * dt;
        m->vel *= expf(-dt * 3.5f);
        if (fabsf(m->vel) < 5) m->vel = 0;
    }
    float lo = 0, hi = (float)(m->strip_h - ML_H);
    if (hi < 0) hi = 0;
    if (m->scroll < lo) { m->scroll = lo; m->vel = 0; }
    if (m->scroll > hi) { m->scroll = hi; m->vel = 0; }
    (void)a;
}

/* a sprite whose anchor is at screen (x, y), no depth */
static void blit(ml_img_t *im, const ml_spr_t *s, int fmt, int x, int y, int alpha)
{
    if (!s) return;
    int x0s = x - s->ax, y0s = y - s->ay;
    int r0 = im->cy0 - y0s, r1 = im->cy1 - y0s;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    int bpp = fmt == ML_PX_COL ? 4 : fmt == ML_PX_IMG ? 3 : fmt == ML_PX_RGB ? 2 : 1;
    for (int r = r0; r < r1; r++) {
        int a0, a1;
        const uint8_t *p = ml_spr_row(s, r, &a0, &a1);
        int c0 = x0s + a0, c1 = x0s + a1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * bpp; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        uint16_t *dst = im->px + (size_t)(y0s + r) * im->w;
        if (fmt == ML_PX_RGB && alpha >= 255) {
            if (c1 > c0) memcpy(dst + c0, p, (size_t)(c1 - c0) * 2);
            continue;
        }
        for (int xx = c0; xx < c1; xx++, p += bpp) {
            uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
            int al = fmt == ML_PX_RGB ? 255 : p[2];
            if (!al) continue;
            if (alpha < 255) al = al * alpha >> 8;
            dst[xx] = ml_blend(dst[xx], c, al);
        }
    }
}

static const ml_spr_t *f0(const ml_anim_t *a)
{
    return a->n ? &a->f[0] : NULL;
}

static void pill_row(app_t *a, ml_img_t *im);

/* the rows of the list, top to bottom as the strip has them (the last world first) */
static int list_world(const app_t *a, int row)
{
    int w = a->worlds.nworlds - 1 - row;
    return w >= 0 ? w : -1;
}

static void put_img(ml_img_t *im, const ml_spr_t *s, int x, int y, int alpha);

/* Lying down: soft colour either side of the strip, and the worlds in a
 * column on the right, each with its emblem, its name and its stars */
static void sides(app_t *a, map_t *m, ml_img_t *im, int y0, int y1)
{
    int mx = MX();
    if (mx <= 0) return;
    for (int y = y0; y < y1; y++) {
        uint16_t c = ml_hex(ml_mix(0x2A1E3E, 0x120C1C, y * 256 / ML_H));
        uint16_t *row = im->px + (size_t)y * im->w;
        int a0 = im->cx0, a1 = mx < im->cx1 ? mx : im->cx1;
        for (int x = a0; x < a1; x++) row[x] = c;
        a0 = mx + STRIP_W > im->cx0 ? mx + STRIP_W : im->cx0;
        for (int x = a0; x < im->cx1; x++) row[x] = c;
    }
    const ml_hud_t *h = &a->hud;
    for (int r = 0; r < a->worlds.nworlds; r++) {
        int w = list_world(a, r);
        if (w < 0) break;
        int top = LIST_Y + r * LIST_ROW;
        if (top > y1 || top + LIST_ROW < y0) continue;
        bool open = mla_world_open(a, w);
        bool here = w == m->cur_w;
        ml_rrect(im, LIST_X - 8, top, ML_W - LIST_X - 16, LIST_ROW - 12, 22, here ? ml_rgb(90, 60, 130) : ml_rgb(52, 38, 72),
                 open ? 230 : 140);
        if (m->emblem[w].n) put_img(im, &m->emblem[w].f[0], LIST_X, top + 2, open ? 255 : 110);
        const ml_mask_t *nm = &a->wname_s[w];
        int tx = LIST_X + 114;
        if (nm->a) ml_mask_draw(im, nm, tx, top + 18, open ? 0xFFFF : ml_rgb(150, 150, 160), 255);
        int st = ml_prog_world_stars(&a->prog, &a->worlds, w), all = a->worlds.w[w].nlevels * 3;
        int sy = top + 18 + (nm->a ? nm->h : 28) + 8 + h->sdig[0].h / 2;
        ml_hud_star(im, tx + 14, sy, 13, open ? ml_rgb(255, 205, 70) : ml_rgb(120, 110, 90));
        int x = tx + 34;
        x += ml_hud_number(h, im, open ? st : a->worlds.w[w].need, x, sy - h->sdig[0].h / 2, false, 0xFFFF, open ? 255 : 150);
        if (open) {
            ml_mask_draw(im, &h->sdig[11], x, sy - h->sdig[0].h / 2, ml_rgb(200, 200, 210), 200);
            x += h->sdig[11].w - 2;
            ml_hud_number(h, im, all, x, sy - h->sdig[0].h / 2, false, ml_rgb(200, 200, 210), 200);
        }
    }
}

static void put_img(ml_img_t *im, const ml_spr_t *s, int x, int y, int alpha)
{
    int r0 = im->cy0 - y, r1 = im->cy1 - y;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    for (int r = r0; r < r1; r++) {
        int a0, a1;
        const uint8_t *p = ml_spr_row(s, r, &a0, &a1);
        int c0 = x + a0, c1 = x + a1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * 3; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        uint16_t *dst = im->px + (size_t)(y + r) * im->w;
        for (int xx = c0; xx < c1; xx++, p += 3) {
            int al = p[2] * alpha >> 8;
            if (al) dst[xx] = ml_blend(dst[xx], (uint16_t)(p[0] | (p[1] << 8)), al);
        }
    }
}

void mlm_band(app_t *a, ml_img_t *im, int y0, int y1)
{
    map_t *m = M(a);
    ml_rect(im, im->cx0, y0, im->cx1 - im->cx0, y1 - y0, 0);
    if (!m) return;
    const ml_hud_t *h = &a->hud;
    int sc = ml_iround(m->scroll);
    int mx = MX();
    for (int i = 0; i < m->np; i++) {
        panel_t *p = &m->p[i];
        int top = p->y - sc;
        if (top >= y1 + 60 || top + p->h + GAP <= y0) continue;
        const ml_spr_t *s = f0(&p->pic);
        if (s) blit(im, s, p->pic.fmt, mx + s->ax, top + s->ay, 255);
        if (!p->open) {
            /* dimmed, with the stars it needs */
            int a0 = top > im->cy0 ? top : im->cy0, a1 = top + p->h < im->cy1 ? top + p->h : im->cy1;
            int xa = mx > im->cx0 ? mx : im->cx0, xb = mx + STRIP_W < im->cx1 ? mx + STRIP_W : im->cx1;
            for (int y = a0; y < a1; y++) {
                uint16_t *row = im->px + (size_t)y * im->w;
                for (int x = xa; x < xb; x++) row[x] = ml_darken(row[x], 90);
            }
            int need = p->world >= 0 ? a->worlds.w[p->world].need : 0;
            int cy = top + p->h / 2;
            int wn = ml_hud_number_w(h, need, true);
            ml_hud_pill(im, ML_W / 2 - wn / 2 - 52, cy - 36, wn + 104, 72, 0, 190);
            ml_hud_star(im, ML_W / 2 - wn / 2 - 18, cy, 20, ml_rgb(255, 205, 70));
            ml_hud_number(h, im, need, ML_W / 2 - wn / 2 + 12, cy - h->dig[0].h / 2, true, 0xFFFF, 255);
        }
        if (p->world >= 0) {
            /* its name on a pill at the panel's top */
            const ml_mask_t *nm = &a->wname[p->world];
            if (nm->a) {
                int ty = top + 34;
                ml_hud_pill(im, mx + 22, ty - nm->h / 2 - 8, nm->w + 36, nm->h + 16, 0, 160);
                ml_mask_draw(im, nm, mx + 40, ty - nm->h / 2, p->open ? 0xFFFF : ml_rgb(170, 170, 180), 255);
            }
            for (int k = 0; k < p->nn && k < a->worlds.w[p->world].nlevels; k++) {
                int x = mx + p->node[k][0], y = top + p->node[k][1];
                if (y < y0 - 110 || y > y1 + 110) continue;
                int stars = a->prog.stars[p->world][k];
                bool open = mla_level_open(a, p->world, k);
                const ml_anim_t *st = stars ? &m->stone_done : open ? &m->stone : &m->stone_locked;
                bool cur = p->world == m->cur_w && k == m->cur_l;
                if (cur && m->ring.n) {
                    int al = 170 + (int)(80 * sinf(m->t * 3));
                    blit(im, f0(&m->ring), m->ring.fmt, x, y, al);
                }
                blit(im, f0(st), st->fmt, x, y, 255);
                if (!st->n) ml_disc(im, x * 16, y * 16, 31 * 16, open ? ml_rgb(250, 235, 220) : ml_rgb(80, 80, 90), 255);
                for (int j = 0; j < stars; j++) ml_hud_star(im, x - 27 + j * 27, y + 46, 11, ml_rgb(255, 205, 70));
                if (cur) {
                    float bob = sinf(m->t * 4) * 4;
                    blit(im, f0(&m->marker), m->marker.fmt, x, y - 12 + (int)bob, 255);
                }
            }
        }
    }
    if (WIDE()) sides(a, m, im, y0, y1);
    pill_row(a, im);
    if (y1 > HOME_Y - HOME_R - 4) ml_hud_button_ico(im, h, ICO_HOME, &h->sym[SYM_HOME], HOME_X, HOME_Y, HOME_R, false);
}

/* the HUD: coins on the left and stars on the right, over the strip */
static int pill_h(const ml_hud_t *h)
{
    int ph = h->sdig[0].h + 16;
    return ph < 44 ? 44 : ph;
}

static void pill_row(app_t *a, ml_img_t *im)
{
    const ml_hud_t *h = &a->hud;
    int ph = pill_h(h), y = 28, cy = y + ph / 2;
    /* upright on the strip's top corners; lying down in the left column,
     * one under the other */
    int l = WIDE() ? 24 : MX() + 20;
    int st = ml_prog_total_stars(&a->prog, &a->worlds);
    int ws = ml_hud_number_w(h, st, false);
    int sx = WIDE() ? l : MX() + STRIP_W - 20 - ws - 70, sy = WIDE() ? y + ph + 14 : y;
    if (im->cy0 >= sy + ph) return;
    ml_hud_pill(im, sx, sy, ws + 70, ph, 0, 170);
    ml_hud_star(im, sx + 28, sy + ph / 2, 14, ml_rgb(255, 205, 70));
    ml_hud_number(h, im, st, sx + 50, sy + ph / 2 - h->sdig[0].h / 2, false, 0xFFFF, 255);
    int wn = ml_hud_number_w(h, a->prog.coins, false);
    ml_hud_pill(im, l, y, wn + 70, ph, 0, 170);
    ml_disc(im, (l + 28) * 16, cy * 16, 14 * 16, ml_rgb(255, 196, 40), 255);
    ml_disc(im, (l + 28) * 16, cy * 16, 8 * 16, ml_rgb(255, 225, 120), 255);
    ml_hud_number(h, im, a->prog.coins, l + 50, cy - h->sdig[0].h / 2, false, ml_rgb(255, 225, 130), 255);
}

/* what changed since the last frame (worker, after mlm_step) */
void mlm_damage(app_t *a, ml_dmg_t *d)
{
    map_t *m = M(a);
    if (!m) return;
    int sc = ml_iround(m->scroll);
    if (sc != m->drawn_sc || m->cur_w != m->drawn_w || m->cur_l != m->drawn_l) {
        m->drawn_sc = sc;
        m->drawn_w = m->cur_w;
        m->drawn_l = m->cur_l;
        ml_dmg_full(d);
        return;
    }
    /* standing: the current stone's ring pulses and Mila bobs on it */
    for (int i = 0; i < m->np; i++) {
        panel_t *p = &m->p[i];
        if (p->world != m->cur_w || m->cur_l >= p->nn) continue;
        int x = MX() + p->node[m->cur_l][0], y = p->y - sc + p->node[m->cur_l][1];
        const ml_spr_t *mk = f0(&m->marker), *rg = f0(&m->ring);
        if (rg) ml_dmg_rect(d, x - rg->ax, y - rg->ay, rg->w, rg->h);
        if (mk) ml_dmg_rect(d, x - mk->ax, y - 12 - 6 - mk->ay, mk->w, mk->h + 12);
    }
    int st = ml_prog_total_stars(&a->prog, &a->worlds);
    if (st != m->drawn_stars || a->prog.coins != m->drawn_coins) {
        m->drawn_stars = st;
        m->drawn_coins = a->prog.coins;
        ml_dmg_rect(d, 0, 20, WIDE() ? MX() : ML_W, 2 * pill_h(&a->hud) + 30);
    }
}

/* the screen turned (worker): the same strip, from the top of the view */
void mlm_refit(app_t *a)
{
    map_t *m = M(a);
    if (!m) return;
    m->drawn_sc = -1;
    float hi = (float)(m->strip_h - ML_H);
    if (m->scroll > hi) m->scroll = hi > 0 ? hi : 0;
}

void mlm_touch(app_t *a, int code, int x, int y)
{
    map_t *m = M(a);
    if (!m) return;
    uint32_t now = lv_tick_get();
    if (code == LV_EVENT_PRESSED) {
        m->press_x = x;
        m->press_y = y;
        m->pressed = true;
        m->dragged = false;
        m->last_y = (float)y;
        m->last_ms = now;
        m->fling = 0;
        m->drag_to = m->scroll;
        m->held = true;
        return;
    }
    if (!m->pressed) return;
    if (code == LV_EVENT_PRESSING) {
        if (abs(y - m->press_y) > 16 || abs(x - m->press_x) > 16) m->dragged = true;
        if (m->dragged) {
            float dy = (float)y - m->last_y;
            m->drag_to -= dy;
            uint32_t dtm = now - m->last_ms;
            if (dtm > 0) m->fling = -dy * 1000.0f / (float)dtm * 0.6f + m->fling * 0.4f;
            m->last_y = (float)y;
            m->last_ms = now;
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        m->pressed = false;
        m->held = false;
        if (now - m->last_ms > 90) m->fling = 0;
        if (m->dragged || code == LV_EVENT_PRESS_LOST) return;
        if ((x - HOME_X) * (x - HOME_X) + (y - HOME_Y) * (y - HOME_Y) < (HOME_R + 14) * (HOME_R + 14)) {
            ml_snd(SND_SELECT);
            mla_set_state(a, ST_CASITA);
            return;
        }
        int sc = ml_iround(m->scroll);
        if (WIDE() && x >= LIST_X - 8) {
            int r = (y - LIST_Y) / LIST_ROW;
            int w = y >= LIST_Y ? list_world(a, r) : -1;
            for (int i = 0; i < m->np && w >= 0 && r < a->worlds.nworlds; i++) {
                if (m->p[i].world != w) continue;
                /* the panel's middle in the middle of the view */
                float to = (float)(m->p[i].y + m->p[i].h / 2 - ML_H / 2);
                float hi = (float)(m->strip_h - ML_H);
                m->go_to = to < 0 ? 0 : to > hi ? hi : to;
                ml_snd(SND_SELECT);
            }
            return;
        }
        for (int i = 0; i < m->np; i++) {
            panel_t *p = &m->p[i];
            int top = p->y - sc;
            if (p->world == -1 && p->house[2] > p->house[0]) {
                if (x >= MX() + p->house[0] && x < MX() + p->house[2] && y >= top + p->house[1] && y < top + p->house[3]) {
                    ml_snd(SND_SELECT);
                    mla_set_state(a, ST_CASITA);
                    return;
                }
            }
            if (p->world < 0) continue;
            for (int k = 0; k < p->nn && k < a->worlds.w[p->world].nlevels; k++) {
                int dx = x - (MX() + p->node[k][0]), dy = y - (top + p->node[k][1]);
                if (dx * dx + dy * dy > 56 * 56) continue;
                if (mla_level_open(a, p->world, k)) {
                    ml_snd(SND_SELECT);
                    mla_level_start(a, p->world, k);
                } else {
                    ml_snd(SND_BUMP);
                }
                return;
            }
        }
    }
}

/* ---- a USB gamepad (LVGL thread) ----
 * Mila's stone is the cursor, as it is the map's marker: the d-pad takes
 * her to the next open level (up or right, the way the strip climbs) or the
 * one before (down or left), L/R to the world before or after, and the strip
 * glides to keep her in view. A or START opens the level she stands on. */

/* the strip's y of a level's stone, or -1 */
static int stone_y(const map_t *m, int w, int l)
{
    for (int i = 0; i < m->np; i++)
        if (m->p[i].world == w && l < m->p[i].nn) return m->p[i].y + m->p[i].node[l][1];
    return -1;
}

void mlm_gamepad(app_t *a, const aos_pad_t *p)
{
    map_t *m = M(a);
    if (!m) return;
    int nw = a->worlds.nworlds;
    int step = aos_pad_repeat(p, AOS_PAD_UP | AOS_PAD_RIGHT) ? 1 : aos_pad_repeat(p, AOS_PAD_DOWN | AOS_PAD_LEFT) ? -1 : 0;
    int jump = aos_pad_pressed(p, AOS_PAD_R) ? 1 : aos_pad_pressed(p, AOS_PAD_L) ? -1 : 0;
    int w = m->cur_w, l = m->cur_l;
    if (step) {
        /* the next open level that way, across the worlds */
        for (;;) {
            l += step;
            if (l < 0) {
                if (--w < 0) break;
                l = a->worlds.w[w].nlevels - 1;
            } else if (l >= a->worlds.w[w].nlevels) {
                if (++w >= nw) break;
                l = 0;
            }
            if (mla_level_open(a, w, l)) break;
        }
    } else if (jump) {
        /* the next open world that way, at its first level not solved */
        for (w += jump; w >= 0 && w < nw && !mla_world_open(a, w); w += jump) {}
        if (w >= 0 && w < nw) {
            l = 0;
            for (int k = 0; k < a->worlds.w[w].nlevels; k++)
                if (mla_level_open(a, w, k) && !a->prog.stars[w][k]) {
                    l = k;
                    break;
                }
        }
    }
    if (step || jump) {
        int y = w >= 0 && w < nw && mla_level_open(a, w, l) ? stone_y(m, w, l) : -1;
        if (y < 0) {
            ml_snd(SND_BUMP);
            return;
        }
        m->cur_w = w;
        m->cur_l = l;
        float to = (float)y - ML_H * 0.58f, hi = (float)(m->strip_h - ML_H);
        m->go_to = to < 0 ? 0 : to > hi ? (hi > 0 ? hi : 0) : to;
        m->vel = 0;
        ml_snd(SND_SELECT);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_A | AOS_PAD_START) && mla_level_open(a, m->cur_w, m->cur_l)) {
        ml_snd(SND_SELECT);
        mla_level_start(a, m->cur_w, m->cur_l);
    }
}
