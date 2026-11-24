/*
 * VITRAL - what goes where on the canvas, and drawing it
 *
 * Two buffers the size of the canvas: the background (bg), with the wall,
 * the frames and the glass that has settled, and the frame (fb, the OS's
 * canvas), which is the background plus what moves: the falling piece, its
 * ghost, the flashes and the shards. Every frame the tiles that held
 * something moving are copied back from bg, what moves is drawn again, and
 * only the tiles touched are presented (vt_gfx.c, "Dirty tiles"). The
 * settled glass is drawn into bg cell by cell, only the cells whose glass,
 * lead or neighbours changed (key[][]).
 */
#include "vt_app.h"
#include "vt_snd.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */

static void layout_single(vt_app_t *a, vt_layout_t *L)
{
    const int W = a->cw, H = a->ch;
    memset(L, 0, sizeof *L);
    L->ax = 0;
    L->ay = 0;
    L->aw = (int16_t)W;
    L->ah = (int16_t)H;
    L->hx = L->hy = -1;
    L->mx = -1;
    if (!a->land) {
        int top = 92, bottom = a->use_keys ? 120 : 40;
        int cell = (H - top - bottom - 6) / VT_VISIBLE;
        int cw = (W - 2 * 54) / VT_COLS_MAX;
        if (cw < cell) cell = cw;
        L->cell = (int16_t)cell;
        int w = cell * VT_COLS_MAX, h = cell * VT_VISIBLE;
        L->wx = (int16_t)((W - w) / 2);
        int extra = H - bottom - top - h - 5;
        L->wy = (int16_t)(top + (extra > 0 ? extra / 3 : 0));
        L->arch = 48;
        L->ncell = (int16_t)(cell >= 20 ? 7 : 6);
        L->nbox = (int16_t)(5 * L->ncell + 6);
        int side = L->wx - 5;
        L->hx = (int16_t)((side - L->nbox) / 2);
        L->hy = (int16_t)(L->wy + 14);
        L->nx = (int16_t)(L->wx + w + 5 + (side - L->nbox) / 2);
        L->ny = (int16_t)(L->wy + 14);
        L->nstep = (int16_t)(L->nbox + 10);
        L->nn = 3;
        if (a->use_keys) {
            L->keys_y = (int16_t)(H - bottom + 10);
            L->keys_h = (int16_t)(bottom - 22);
            int kw = (W - 8 - 4 * 4 - 18) / 6;
            for (int i = 0; i < 3; i++) {
                L->key_x[i] = (int16_t)(4 + i * (kw + 4));
                L->key_x[5 - i] = (int16_t)(W - 4 - kw - i * (kw + 4));
            }
            for (int i = 0; i < 6; i++) {
                L->key_w[i] = (int16_t)kw;
                L->kys[i] = L->keys_y;
            }
        }
    } else {
        int top = 38, bottom = 8;
        int cell = (H - top - bottom - 5) / VT_VISIBLE;
        L->cell = (int16_t)cell;
        int w = cell * VT_COLS_MAX, h = cell * VT_VISIBLE;
        L->wx = (int16_t)((W - w) / 2);
        L->wy = (int16_t)(H - bottom - 5 - h);
        L->arch = 28;
        L->ncell = 6;
        L->nbox = (int16_t)(5 * L->ncell + 6);
        L->hx = (int16_t)(L->wx - 5 - 14 - L->nbox);
        L->hy = (int16_t)(L->wy + 14);
        L->nx = (int16_t)(L->wx + w + 5 + 14);
        L->ny = (int16_t)(L->wy + 14);
        L->nstep = (int16_t)(L->nbox + 8);
        L->nn = a->use_keys ? 3 : 4;
        if (a->use_keys) {
            /* lying down: three keys low on each side */
            int kh = 66, ky = H - 10 - kh;
            int lw = (L->hx - 16) / 3 - 4;
            int rx0 = L->nx + L->nbox + 14, rw = (W - 8 - rx0) / 3 - 4;
            if (lw > 64) lw = 64;
            if (rw > 64) rw = 64;
            L->keys_y = (int16_t)ky;
            L->keys_h = (int16_t)kh;
            for (int i = 0; i < 3; i++) {
                L->key_x[i] = (int16_t)(8 + i * (lw + 4));
                L->key_w[i] = (int16_t)lw;
                L->key_x[3 + i] = (int16_t)(W - 8 - (3 - i) * (rw + 4) + 4);
                L->key_w[3 + i] = (int16_t)rw;
            }
            for (int i = 0; i < 6; i++) L->kys[i] = (int16_t)ky;
        }
    }
}

static void layout_versus(vt_app_t *a, vt_layout_t *L, int pi)
{
    memset(L, 0, sizeof *L);
    L->hx = L->hy = -1;
    if (a->land) {
        L->ax = (int16_t)(pi * (a->cw / 2));
        L->ay = 0;
        L->aw = (int16_t)(a->cw / 2);
        L->ah = (int16_t)a->ch;
        L->flip = false;
        int top = 38, bottom = 8;
        int cell = (L->ah - top - bottom - 5) / VT_VISIBLE;
        L->cell = (int16_t)cell;
        int w = cell * VT_COLS_MAX, h = cell * VT_VISIBLE;
        L->wx = (int16_t)(L->aw / 2 - w / 2 - 10);
        L->wy = (int16_t)(L->ah - bottom - 5 - h);
        L->arch = 28;
        L->ncell = 6;
        L->nbox = (int16_t)(5 * L->ncell + 6);
        L->nx = (int16_t)(L->wx + w + 5 + 9);
        L->ny = (int16_t)(L->wy + 14);
        L->nstep = (int16_t)(L->nbox + 8);
        L->nn = 3;
        L->mx = (int16_t)(L->wx - 5 - 12);
    } else {
        L->ax = 0;
        L->aw = (int16_t)a->cw;
        L->ah = (int16_t)(a->ch / 2);
        L->ay = (int16_t)(pi == 0 ? a->ch / 2 : 0);
        L->flip = pi == 1;
        int top = 26, bottom = 6;
        int cell = (L->ah - top - bottom - 5) / VT_VISIBLE;
        L->cell = (int16_t)cell;
        int w = cell * VT_COLS_MAX, h = cell * VT_VISIBLE;
        L->wx = (int16_t)((L->aw - w) / 2);
        L->wy = (int16_t)(L->ah - bottom - 5 - h);
        L->arch = 18;
        L->ncell = 5;
        L->nbox = (int16_t)(5 * L->ncell + 6);
        L->nx = (int16_t)(L->wx + w + 5 + 12);
        L->ny = (int16_t)(L->wy + 14);
        L->nstep = (int16_t)(L->nbox + 8);
        L->nn = 3;
        L->mx = (int16_t)(L->wx - 5 - 12);
    }
}

void vt_view_layout(vt_app_t *a)
{
    vt_dirty_init(&a->d_prev, a->cw, a->ch);
    vt_dirty_init(&a->d_cur, a->cw, a->ch);
    vt_dirty_init(&a->d_static, a->cw, a->ch);
    for (int i = 0; i < 2; i++) {
        vt_player_t *p = &a->p[i];
        if (a->nplayers == 2) layout_versus(a, &p->L, i);
        else layout_single(a, &p->L);
        vt_surf_init(&p->sbg, a->bg, a->cw, a->ch, p->L.ax, p->L.ay, p->L.aw, p->L.ah, p->L.flip);
        vt_surf_init(&p->sfb, a->r->px, a->cw, a->ch, p->L.ax, p->L.ay, p->L.aw, p->L.ah, p->L.flip);
        p->board_all = true;
        p->previews_dirty = true;
    }
}

/* -------------------------------------------------------------------------- */
/* Cells                                                                       */

static vt_surf_t well_surf(const vt_player_t *p, bool fb)
{
    const vt_layout_t *L = &p->L;
    return vt_sub(fb ? &p->sfb : &p->sbg, L->wx, L->wy, L->cell * p->g.cols, L->cell * VT_VISIBLE);
}

static bool same_piece(const vt_game_t *g, int x, int y, int ox, int oy)
{
    if (ox < 0 || ox >= g->cols || oy < 0 || oy >= VT_ROWS) return false;
    if (g->col[oy][ox] == VT_EMPTY) return false;
    return g->pid[oy][ox] == g->pid[y][x];
}

static uint8_t lead_of(const vt_game_t *g, int x, int y)
{
    uint8_t l = 0;
    bool t = same_piece(g, x, y, x, y - 1), b = same_piece(g, x, y, x, y + 1);
    bool lf = same_piece(g, x, y, x - 1, y), rt = same_piece(g, x, y, x + 1, y);
    if (!t) l |= VT_L_TOP;
    if (!b) l |= VT_L_BOTTOM;
    if (!lf) l |= VT_L_LEFT;
    if (!rt) l |= VT_L_RIGHT;
    if (t && lf && !same_piece(g, x, y, x - 1, y - 1)) l |= VT_L_TL;
    if (t && rt && !same_piece(g, x, y, x + 1, y - 1)) l |= VT_L_TR;
    if (b && rt && !same_piece(g, x, y, x + 1, y + 1)) l |= VT_L_BR;
    if (b && lf && !same_piece(g, x, y, x - 1, y + 1)) l |= VT_L_BL;
    return l;
}

static uint8_t mask_edge(const vt_game_t *g, int x, int y)
{
    uint8_t e = 0;
    if (y == 0 || !g->mask[y - 1][x]) e |= VT_L_TOP;
    if (y == VT_ROWS - 1 || !g->mask[y + 1][x]) e |= VT_L_BOTTOM;
    if (x == 0 || !g->mask[y][x - 1]) e |= VT_L_LEFT;
    if (x == g->cols - 1 || !g->mask[y][x + 1]) e |= VT_L_RIGHT;
    return e;
}

static uint32_t seed_of(uint16_t pid)
{
    return (uint32_t)pid * 2654435761u;
}

/* the lead of a cell of a lone piece (the falling one, the previews) */
static uint8_t piece_lead(const vt_shape_t *s, int rot, int i)
{
    uint8_t l = VT_L_TOP | VT_L_BOTTOM | VT_L_LEFT | VT_L_RIGHT;
    int x = s->c[rot][i].x, y = s->c[rot][i].y;
    bool has[3][3] = { { 0 } };
    for (int j = 0; j < s->n; j++) {
        int dx = s->c[rot][j].x - x, dy = s->c[rot][j].y - y;
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) has[dy + 1][dx + 1] = true;
    }
    if (has[0][1]) l &= (uint8_t)~VT_L_TOP;
    if (has[2][1]) l &= (uint8_t)~VT_L_BOTTOM;
    if (has[1][0]) l &= (uint8_t)~VT_L_LEFT;
    if (has[1][2]) l &= (uint8_t)~VT_L_RIGHT;
    if (has[0][1] && has[1][0] && !has[0][0]) l |= VT_L_TL;
    if (has[0][1] && has[1][2] && !has[0][2]) l |= VT_L_TR;
    if (has[2][1] && has[1][2] && !has[2][2]) l |= VT_L_BR;
    if (has[2][1] && has[1][0] && !has[2][0]) l |= VT_L_BL;
    return l;
}

static void mark(vt_app_t *a, vt_dirty_t *d, const vt_surf_t *s, int x, int y, int w, int h)
{
    (void)a;
    vt_dirty_add(d, vt_surf_rect(s, x, y, w, h));
}

static void draw_board(vt_app_t *a, vt_player_t *p)
{
    vt_game_t *g = &p->g;
    vt_surf_t ws = well_surf(p, false);
    int c = p->L.cell;
    for (int y = VT_HIDDEN; y < VT_ROWS; y++) {
        for (int x = 0; x < g->cols; x++) {
            int col = g->col[y][x];
            uint32_t k;
            uint8_t l = 0;
            if (col == VT_EMPTY) {
                l = g->mask[y][x] ? mask_edge(g, x, y) : 0;
                k = 0x80000000u | (g->mask[y][x] ? 0x100u : 0u) | l;
            } else if (col == VT_STONE) {
                k = 0x40000000u;
            } else {
                l = lead_of(g, x, y);
                k = (uint32_t)col | ((uint32_t)l << 8) | ((uint32_t)(g->pid[y][x] & 0x3FFF) << 16);
            }
            if (!p->board_all && p->key[y][x] == k) continue;
            p->key[y][x] = k;
            int px = x * c, py = (y - VT_HIDDEN) * c;
            if (col == VT_EMPTY) vt_empty_cell(&ws, px, py, c, px, py, g->mask[y][x] != 0, l);
            else vt_glass_cell(&ws, px, py, c, col, l, px, py, col == VT_STONE ? 99 : seed_of(g->pid[y][x]), 0);
            vt_dirty_add(&a->d_static, vt_surf_rect(&ws, px, py, c, c));
        }
    }
    p->board_all = false;
}

static void draw_mini(const vt_surf_t *s, int bx, int by, int box, int nc, int type, int color, uint32_t seed)
{
    if (type < 0 || type >= PC_COUNT) return;
    const vt_shape_t *sh = &vt_shapes[type];
    int x0 = 9, x1 = -1, y0 = 9, y1 = -1;
    for (int i = 0; i < sh->n; i++) {
        if (sh->c[0][i].x < x0) x0 = sh->c[0][i].x;
        if (sh->c[0][i].x > x1) x1 = sh->c[0][i].x;
        if (sh->c[0][i].y < y0) y0 = sh->c[0][i].y;
        if (sh->c[0][i].y > y1) y1 = sh->c[0][i].y;
    }
    int pw = (x1 - x0 + 1) * nc, ph = (y1 - y0 + 1) * nc;
    int ox = bx + (box - pw) / 2, oy = by + (box - ph) / 2;
    for (int i = 0; i < sh->n; i++) {
        int cx = ox + (sh->c[0][i].x - x0) * nc, cy = oy + (sh->c[0][i].y - y0) * nc;
        vt_glass_cell(s, cx, cy, nc, color, piece_lead(sh, 0, i), cx, cy, seed, 10);
    }
}

static void draw_previews(vt_app_t *a, vt_player_t *p)
{
    vt_game_t *g = &p->g;
    const vt_layout_t *L = &p->L;
    uint8_t now[VT_QUEUE * 2 + 2];
    memset(now, 0xFE, sizeof now);
    for (int i = 0; i < L->nn && i < VT_QUEUE; i++) {
        now[2 * i] = g->queue[i].type;
        now[2 * i + 1] = g->queue[i].color;
    }
    now[VT_QUEUE * 2] = g->hold.type;
    now[VT_QUEUE * 2 + 1] = (uint8_t)(g->hold.color | (g->hold_used ? 0x80 : 0));
    if (!p->previews_dirty && !memcmp(now, p->shown_next, sizeof now)) return;
    p->previews_dirty = false;
    memcpy(p->shown_next, now, sizeof now);
    for (int i = 0; i < L->nn; i++) {
        int bx = L->nx, by = L->ny + i * L->nstep;
        /* the first one in a bigger box would not fit the columns: all one size */
        vt_box(&p->sbg, bx, by, L->nbox, L->nbox);
        if (g->queue[i].type < PC_COUNT)
            draw_mini(&p->sbg, bx, by, L->nbox, L->ncell, g->queue[i].type, g->queue[i].color, 1234u + (uint32_t)i * 77u);
        mark(a, &a->d_static, &p->sbg, bx - 3, by - 3, L->nbox + 6, L->nbox + 6);
    }
    if (L->hx >= 0 && g->mode != VT_MODE_PUZZLE) {
        vt_box(&p->sbg, L->hx, L->hy, L->nbox, L->nbox);
        if (g->hold.type < PC_COUNT) {
            draw_mini(&p->sbg, L->hx, L->hy, L->nbox, L->ncell, g->hold.type, g->hold.color, 999);
            if (g->hold_used) vt_tint(&p->sbg, L->hx, L->hy, L->nbox, L->nbox, vt_rgb(10, 11, 22), 150);
        }
        mark(a, &a->d_static, &p->sbg, L->hx - 3, L->hy - 3, L->nbox + 6, L->nbox + 6);
    }
}

/* the meter of rubble coming up (versus) */
static void draw_meter(vt_app_t *a, vt_player_t *p, const vt_surf_t *s, bool dyn)
{
    const vt_layout_t *L = &p->L;
    if (L->mx < 0) return;
    int h = L->cell * VT_VISIBLE, x = L->mx, y = L->wy;
    int n = p->g.garbage_in;
    int fill = n * L->cell;
    if (fill > h) fill = h;
    vt_fill(s, x, y, 7, h, vt_rgb(14, 14, 24));
    if (fill) vt_fill(s, x + 1, y + h - fill, 5, fill, vt_hex(0xD04030));
    if (dyn) mark(a, &a->d_cur, s, x, y, 7, h);
}

static void draw_keys(vt_app_t *a, vt_player_t *p, uint8_t down)
{
    const vt_layout_t *L = &p->L;
    if (!L->keys_h) return;
    static const int G[6] = { VT_GLYPH_LEFT, VT_GLYPH_DOWN, VT_GLYPH_RIGHT, VT_GLYPH_DROP, VT_GLYPH_CCW, VT_GLYPH_CW };
    for (int i = 0; i < 6; i++) {
        vt_key(&p->sbg, L->key_x[i], L->kys[i], L->key_w[i], L->keys_h, G[i], (down >> i) & 1);
        mark(a, &a->d_static, &p->sbg, L->key_x[i], L->kys[i], L->key_w[i], L->keys_h);
    }
}

/* -------------------------------------------------------------------------- */
/* Whole backgrounds                                                           */

static void present_all(vt_app_t *a)
{
    memcpy(a->r->px, a->bg, (size_t)a->cw * (size_t)a->ch * 2);
    aos_retro_present();
    vt_dirty_clear(&a->d_prev);
    vt_dirty_clear(&a->d_static);
}

void vt_view_title(vt_app_t *a)
{
    vt_surf_t s;
    vt_surf_init(&s, a->bg, a->cw, a->ch, 0, 0, a->cw, a->ch, false);
    vt_wall(&s);
    if (!a->land) vt_rose(&s, a->cw / 2, 178, 150);
    else vt_rose(&s, 186, a->ch / 2, 150);
    a->nparts = 0;
    present_all(a);
}

void vt_view_game(vt_app_t *a)
{
    vt_surf_t s;
    vt_surf_init(&s, a->bg, a->cw, a->ch, 0, 0, a->cw, a->ch, false);
    vt_wall(&s);
    for (int i = 0; i < a->nplayers; i++) {
        vt_player_t *p = &a->p[i];
        const vt_layout_t *L = &p->L;
        int w = L->cell * p->g.cols, h = L->cell * VT_VISIBLE;
        vt_frame(&p->sbg, L->wx, L->wy, w, h, L->arch, false);
        p->board_all = true;
        p->previews_dirty = true;
        draw_board(a, p);
        draw_previews(a, p);
        draw_meter(a, p, &p->sbg, false);
        draw_keys(a, p, 0);
        a->keys_shown[i] = 0;
    }
    a->nparts = 0;
    present_all(a);
}

/* -------------------------------------------------------------------------- */
/* Particles                                                                   */

static float frand(void)
{
    static uint32_t s = 0x13579BDu;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFF) / 65535.0f;
}

static void part_add(vt_app_t *a, float x, float y, float vx, float vy, float g, uint16_t col, int life,
                     int size, int kind)
{
    if (a->nparts >= VT_MAX_PARTS) return;
    vt_part_t *q = &a->parts[a->nparts++];
    q->x = x;
    q->y = y;
    q->vx = vx;
    q->vy = vy;
    q->g = g;
    q->color = col;
    q->life = q->max = (uint8_t)(life > 255 ? 255 : life);
    q->size = (uint8_t)size;
    q->kind = (uint8_t)kind;
}

/* a cell of glass breaking: shards that fly up and fall, a few sparks */
static void shatter_cell(vt_app_t *a, vt_player_t *p, int x, int y, int color, bool vit)
{
    vt_surf_t ws = well_surf(p, true);
    int c = p->L.cell;
    float sgn = ws.flip ? -1.0f : 1.0f;
    float scale = (float)c / 21.0f;
    uint32_t rgb = vt_glass_rgb(color);
    int n = vit ? 6 : 4;
    for (int k = 0; k < n; k++) {
        int lx = x * c + (int)(frand() * (float)c), ly = (y - VT_HIDDEN) * c + (int)(frand() * (float)c);
        int cx, cy;
        vt_to_canvas(&ws, lx, ly, &cx, &cy);
        float vx = (frand() - 0.5f) * 5.0f * scale, vy = (-1.0f - frand() * 3.2f) * scale * sgn;
        int lift = (int)(frand() * 90.0f) - 30;
        uint16_t col = vt_rgb((int)((rgb >> 16) & 255) + lift, (int)((rgb >> 8) & 255) + lift, (int)(rgb & 255) + lift);
        part_add(a, (float)cx, (float)cy, vx, vy, 0.22f * scale * sgn, col, 34 + (int)(frand() * 30.0f),
                 2 + (int)(frand() * 2.5f * scale), 0);
    }
    if (vit) {
        int cx, cy;
        vt_to_canvas(&ws, x * c + c / 2, (y - VT_HIDDEN) * c + c / 2, &cx, &cy);
        for (int k = 0; k < 3; k++)
            part_add(a, (float)cx, (float)cy, (frand() - 0.5f) * 3.0f, (frand() - 0.8f) * 3.0f * sgn, 0.02f * sgn,
                     vt_hex(0xFFF2C0), 50 + (int)(frand() * 40.0f), 2, 1);
    }
}

static void parts_step(vt_app_t *a)
{
    int j = 0;
    for (int i = 0; i < a->nparts; i++) {
        vt_part_t *q = &a->parts[i];
        q->vy += q->g;
        q->x += q->vx;
        q->y += q->vy;
        q->vx *= 0.99f;
        if (!q->life || q->x < -8 || q->x > a->cw + 8 || q->y < -8 || q->y > a->ch + 8) continue;
        q->life--;
        a->parts[j++] = *q;
    }
    a->nparts = j;
}

static void parts_draw(vt_app_t *a)
{
    vt_surf_t s;
    vt_surf_init(&s, a->r->px, a->cw, a->ch, 0, 0, a->cw, a->ch, false);
    for (int i = 0; i < a->nparts; i++) {
        vt_part_t *q = &a->parts[i];
        int x = (int)q->x, y = (int)q->y, sz = q->size;
        int f = q->life * 256 / (q->max ? q->max : 1);
        if (q->kind == 1) {
            /* a spark: a little cross of light that twinkles */
            int tw = (q->life >> 2) & 1 ? 255 : 150;
            int k = f * tw / 256;
            for (int d = -sz; d <= sz; d++) {
                if (x + d >= 0 && x + d < a->cw && y >= 0 && y < a->ch)
                    s.px[y * a->cw + x + d] = vt_add(s.px[y * a->cw + x + d], q->color, k);
                if (d && x >= 0 && x < a->cw && y + d >= 0 && y + d < a->ch)
                    s.px[(y + d) * a->cw + x] = vt_add(s.px[(y + d) * a->cw + x], q->color, k);
            }
            vt_dirty_add(&a->d_cur, vt_surf_rect(&s, x - sz, y - sz, 2 * sz + 1, 2 * sz + 1));
            continue;
        }
        /* a shard: a sliver with a bright edge, turning as it falls */
        bool tall = ((q->life >> 3) + i) & 1;
        int w = tall ? sz : sz + 1, h = tall ? sz + 1 : sz;
        uint16_t c = q->color;
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                int X = x + xx, Y = y + yy;
                if (X < 0 || Y < 0 || X >= a->cw || Y >= a->ch) continue;
                uint16_t *d = &s.px[Y * a->cw + X];
                uint16_t pc = (xx == 0 && yy == 0) ? vt_add(c, vt_hex(0xFFFFFF), 120) : c;
                *d = f > 120 ? pc : vt_mix(*d, pc, f * 2);
            }
        }
        vt_dirty_add(&a->d_cur, vt_surf_rect(&s, x, y, w, h));
    }
}

/* -------------------------------------------------------------------------- */
/* What moves                                                                  */

static void draw_piece_at(vt_app_t *a, vt_player_t *p, const vt_surf_t *ws, const vt_piece_t *pc, int dy_px,
                          int light, bool ghost)
{
    const vt_shape_t *sh = &vt_shapes[pc->type];
    int c = p->L.cell;
    for (int i = 0; i < sh->n; i++) {
        int x = (pc->x + sh->c[pc->rot][i].x) * c;
        int y = (pc->y + sh->c[pc->rot][i].y - VT_HIDDEN) * c + dy_px;
        if (y + c <= 0) continue;
        uint8_t l = piece_lead(sh, pc->rot, i);
        if (ghost) vt_ghost_cell(ws, x, y, c, pc->color, l);
        else vt_glass_cell(ws, x, y, c, pc->color, l, x, y - dy_px, seed_of(pc->pid), light);
        vt_dirty_add(&a->d_cur, vt_surf_rect(ws, x, y, c, c));
    }
}

static void glow_cells(vt_app_t *a, vt_player_t *p, const vt_surf_t *ws, const vt_piece_t *pc, uint16_t col, int f)
{
    const vt_shape_t *sh = &vt_shapes[pc->type];
    int c = p->L.cell;
    for (int i = 0; i < sh->n; i++) {
        int x = (pc->x + sh->c[pc->rot][i].x) * c, y = (pc->y + sh->c[pc->rot][i].y - VT_HIDDEN) * c;
        vt_dirty_add(&a->d_cur, vt_glow(ws, x, y, c, c, col, f));
    }
}

static void draw_dynamic(vt_app_t *a, vt_player_t *p)
{
    vt_game_t *g = &p->g;
    vt_surf_t ws = well_surf(p, true);
    int c = p->L.cell;

    /* the rows about to break: they light up, a vitral in gold */
    if (g->state == VT_CLEARING && g->clear_t < VT_CLEAR_FLASH_MS) {
        int t = (int)g->clear_t * 256 / VT_CLEAR_FLASH_MS;
        for (int i = 0; i < g->clear_n; i++) {
            int y = (g->clear_row[i] - VT_HIDDEN) * c;
            bool vit = g->clear_vit[i] != 0;
            uint16_t col = vit ? vt_hex(0xFFD878) : vt_hex(0xFFFFFF);
            vt_dirty_add(&a->d_cur, vt_glow(&ws, 0, y, c * g->cols, c, col, 40 + t * 3 / 4));
            if (vit) {
                vt_dirty_add(&a->d_cur, vt_glow(&ws, 0, y - c / 2, c * g->cols, c / 2, col, t / 3));
                vt_dirty_add(&a->d_cur, vt_glow(&ws, 0, y + c, c * g->cols, c / 2, col, t / 3));
            }
        }
    }

    if (g->has_cur && g->state == VT_PLAY && !a->paused) {
        if (a->ghost) {
            vt_piece_t gp = g->cur;
            gp.y = (int8_t)vt_ghost_y(g);
            if (gp.y != g->cur.y) draw_piece_at(a, p, &ws, &gp, 0, 0, true);
        }
        int frac = vt_fall_frac(g) * c / 256;
        draw_piece_at(a, p, &ws, &g->cur, frac, 26, false);
    } else if (g->has_cur && g->state == VT_PLAY) {
        draw_piece_at(a, p, &ws, &g->cur, 0, 0, false);
    }

    /* the drop's trail: light down the columns the piece fell through */
    if (p->trail_t) {
        const vt_shape_t *sh = &vt_shapes[p->trail_p.type];
        uint16_t col = vt_hex(vt_glass_rgb(p->trail_p.color));
        int f0 = p->trail_t * 14;
        for (int i = 0; i < sh->n; i++) {
            int cx = p->trail_p.x + sh->c[p->trail_p.rot][i].x, cy = p->trail_p.y + sh->c[p->trail_p.rot][i].y;
            bool top = true;
            for (int j = 0; j < sh->n; j++)
                if (p->trail_p.x + sh->c[p->trail_p.rot][j].x == cx && p->trail_p.y + sh->c[p->trail_p.rot][j].y < cy)
                    top = false;
            if (!top) continue;
            int y1 = (cy - VT_HIDDEN) * c, y0 = (p->trail_from + sh->c[p->trail_p.rot][i].y - VT_HIDDEN) * c;
            if (y0 < 0) y0 = 0;
            for (int y = y0; y < y1; y++) {
                int f = f0 * (y - y0 + 4) / (y1 - y0 + 4);
                vt_glow(&ws, cx * c + 3, y, c - 6, 1, col, f);
            }
            vt_dirty_add(&a->d_cur, vt_surf_rect(&ws, cx * c, y0, c, y1 - y0));
        }
    }

    /* the piece that just settled flashes */
    if (p->lock_t) glow_cells(a, p, &ws, &p->lock_p, vt_hex(0xFFFFFF), p->lock_t * 12);

    /* a piece that does not fit there (puzzle): it blinks red where it was */
    if (p->reject_t) {
        vt_piece_t rp = p->reject_p;
        if ((p->reject_t >> 2) & 1) {
            draw_piece_at(a, p, &ws, &rp, 0, 0, false);
            glow_cells(a, p, &ws, &rp, vt_hex(0xFF2010), 120);
        }
    }

    /* after a vitral: beams of coloured light across the window */
    if (p->ray_t) {
        int t = p->ray_t;
        int f = t > 40 ? (60 - t) * 6 : t * 3;
        if (f > 120) f = 120;
        int w = c * g->cols, h = c * VT_VISIBLE;
        for (int k = 0; k < 4; k++) {
            int base = w * (k * 2 + 1) / 8 + (60 - t) * (k & 1 ? 1 : -1);
            for (int y = 0; y < h; y += 1) {
                int xc = base + (h - y) / 3 - h / 6;
                vt_glow(&ws, xc - 5, y, 10, 1, p->ray_col, f);
                vt_glow(&ws, xc - 9, y, 4, 1, p->ray_col, f / 3);
                vt_glow(&ws, xc + 5, y, 4, 1, p->ray_col, f / 3);
            }
        }
        vt_dirty_add(&a->d_cur, vt_surf_rect(&ws, 0, 0, w, h));
    }

    /* the hint (puzzle): where the generator put this piece */
    if (a->hint_t && g->mode == VT_MODE_PUZZLE && g->has_cur && g->pz_i < g->pz_n) {
        vt_piece_t hp = g->cur;
        hp.x = g->pz_sol[g->pz_i].x;
        hp.y = g->pz_sol[g->pz_i].y;
        hp.rot = g->pz_sol[g->pz_i].rot;
        if ((a->hint_t >> 3) & 1) draw_piece_at(a, p, &ws, &hp, 0, 0, true);
        else glow_cells(a, p, &ws, &hp, vt_hex(0xFFE9A0), 60);
    }

    if (g->mode == VT_MODE_VERSUS) draw_meter(a, p, &p->sfb, true);
}

/* -------------------------------------------------------------------------- */

void vt_view_events(vt_app_t *a, int pi)
{
    vt_player_t *p = &a->p[pi];
    vt_game_t *g = &p->g;
    uint32_t ev = g->ev;
    if (!ev) return;
    int vol = a->nplayers == 2 ? 200 : 256;

    if (ev & EV_HARD) {
        p->trail_t = 9;
        p->trail_p = g->fx_piece;
        p->trail_from = g->fx_hard_from;
        vt_snd_play(VT_SND_HARD, vol);
    } else if (ev & EV_LOCK) {
        vt_snd_play(VT_SND_LOCK, vol);
    }
    if (ev & EV_LOCK) {
        p->lock_t = 10;
        p->lock_p = g->fx_piece;
    }
    if (ev & EV_ROTATE) vt_snd_play(VT_SND_ROTATE, vol * 3 / 4);
    if (ev & EV_MOVE) vt_snd_play(VT_SND_MOVE, vol / 2);
    if (ev & EV_SOFT) vt_snd_play(VT_SND_SOFT, vol / 3);
    if (ev & EV_HOLD) vt_snd_play(VT_SND_HOLD, vol);
    if (ev & EV_CLEAR) {
        int n = g->fx_rows_n;
        if (ev & EV_VITRAL) vt_snd_play(VT_SND_VITRAL, vol);
        else vt_snd_play(n >= 4 ? VT_SND_CLEAR3 : n >= 2 ? VT_SND_CLEAR2 : VT_SND_CLEAR1, vol);
        /* the points float up */
        char buf[40];
        if (ev & EV_VITRAL) snprintf(buf, sizeof buf, "%s +%u", _("¡Vitral!"), (unsigned)g->fx_points);
        else snprintf(buf, sizeof buf, "+%u", (unsigned)g->fx_points);
        if (p->lb_pop) {
            lv_label_set_text(p->lb_pop, buf);
            lv_obj_set_style_text_color(p->lb_pop, lv_color_hex(ev & EV_VITRAL ? 0xFFD870 : 0xFFFFFF), 0);
            lv_obj_remove_flag(p->lb_pop, LV_OBJ_FLAG_HIDDEN);
            p->pop_until = a->now + 900;
        }
    }
    if (ev & EV_SHATTER) {
        /* the rows that went, as they were */
        for (int i = 0; i < g->fx_rows_n; i++) {
            for (int x = 0; x < g->cols; x++) {
                int col = g->fx_rows_col[i][x];
                if (col != VT_EMPTY) shatter_cell(a, p, x, g->fx_rows[i], col, g->fx_rows_vit[i] != 0);
            }
            if (g->fx_rows_vit[i] == 1) {
                int col = VT_CLEAR;
                for (int x = 0; x < g->cols; x++)
                    if (g->fx_rows_col[i][x] != VT_CLEAR) { col = g->fx_rows_col[i][x]; break; }
                p->ray_t = 60;
                uint32_t rgb = vt_glass_rgb(col);
                p->ray_col = vt_rgb((int)((rgb >> 16) & 255) / 2 + 120, (int)((rgb >> 8) & 255) / 2 + 120,
                                    (int)(rgb & 255) / 2 + 120);
            }
        }
    }
    if (ev & EV_LEVEL) {
        vt_snd_play(VT_SND_LEVEL, vol);
        p->level_t = 60;
    }
    if (ev & EV_REJECT) {
        p->reject_t = 16;
        p->reject_p = g->fx_piece;
        vt_snd_play(VT_SND_REJECT, vol);
    }
    if (ev & EV_GARBAGE) vt_snd_play(VT_SND_GARBAGE, vol);
    if (ev & EV_WON) {
        vt_snd_play(VT_SND_WIN, 256);
        vt_view_burst(a, pi);
    }
    if (ev & EV_UNDO) vt_snd_play(VT_SND_TAP, vol);
    if (ev & EV_KICKFAIL) vt_snd_play(VT_SND_TICK, vol / 3);
    if (ev & EV_OVER) {
        vt_snd_play(VT_SND_OVER, 256);
        /* the glass of the well goes in a last shower */
        for (int y = VT_HIDDEN; y < VT_ROWS; y += 2)
            for (int x = 0; x < g->cols; x += 2)
                if (g->col[y][x] != VT_EMPTY && g->col[y][x] != VT_STONE && a->nparts < VT_MAX_PARTS - 8)
                    shatter_cell(a, p, x, y, g->col[y][x], false);
    }
}

void vt_view_burst(vt_app_t *a, int pi)
{
    vt_player_t *p = &a->p[pi];
    vt_game_t *g = &p->g;
    vt_surf_t ws = well_surf(p, true);
    int c = p->L.cell;
    float sgn = ws.flip ? -1.0f : 1.0f;
    for (int y = VT_HIDDEN; y < VT_ROWS; y++) {
        for (int x = 0; x < g->cols; x++) {
            if (!g->mask[y][x]) continue;
            int cx, cy;
            vt_to_canvas(&ws, x * c + c / 2, (y - VT_HIDDEN) * c + c / 2, &cx, &cy);
            part_add(a, (float)cx, (float)cy, (frand() - 0.5f) * 2.5f, (-0.5f - frand() * 2.0f) * sgn, 0.015f * sgn,
                     vt_hex(0xFFF0C0), 60 + (int)(frand() * 60.0f), 2, 1);
        }
    }
    p->ray_t = 60;
    p->ray_col = vt_hex(0xFFE8A8);
}

/* -------------------------------------------------------------------------- */
/* The frame                                                                   */

void vt_view_frame(vt_app_t *a)
{
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    vt_dirty_clear(&a->d_cur);
    if (a->screen == SCR_GAME) {
        for (int i = 0; i < a->nplayers; i++) {
            vt_player_t *p = &a->p[i];
            draw_board(a, p);
            draw_previews(a, p);
            if (p->L.keys_h && a->keys_shown[i] != p->keys_down) {
                a->keys_shown[i] = p->keys_down;
                draw_keys(a, p, p->keys_down);
            }
        }
    }

    /* back to the background where something moved or changed */
    vt_dirty_t back = a->d_prev;
    vt_dirty_or(&back, &a->d_static);
    vt_rect_t rr[40];
    int n = vt_dirty_rects(&back, rr, 40);
    for (int i = 0; i < n; i++) vt_copy(a->r->px, a->bg, a->cw, rr[i]);

    if (a->screen == SCR_GAME) {
        for (int i = 0; i < a->nplayers; i++) draw_dynamic(a, &a->p[i]);
    }
    if (!a->paused) parts_step(a);
    parts_draw(a);

    vt_dirty_or(&back, &a->d_cur);
    n = vt_dirty_rects(&back, rr, 40);
    for (int i = 0; i < n; i++) aos_retro_present_rect(rr[i].x, rr[i].y, rr[i].w, rr[i].h);
    a->d_prev = a->d_cur;
    vt_dirty_clear(&a->d_static);

    /* effects count down by frames */
    if (!a->paused) {
        for (int i = 0; i < a->nplayers; i++) {
            vt_player_t *p = &a->p[i];
            if (p->lock_t) p->lock_t--;
            if (p->trail_t) p->trail_t--;
            if (p->ray_t) p->ray_t--;
            if (p->reject_t) p->reject_t--;
            if (p->level_t) p->level_t--;
        }
        if (a->hint_t) a->hint_t--;
    }

    uint32_t dt = (uint32_t)aos_hal_uptime_us() - t0;
    a->draw_us += dt;
    if (dt > a->draw_max) a->draw_max = dt;
}

/* -------------------------------------------------------------------------- */
/* The labels                                                                  */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, "");
    return l;
}

/* places a label over a box of the player's surface, turned with it */
static void place(vt_app_t *a, vt_player_t *p, lv_obj_t *l, int x, int y, int w, int h, lv_text_align_t al)
{
    int cx0, cy0, cx1, cy1;
    vt_to_canvas(&p->sbg, x, y, &cx0, &cy0);
    vt_to_canvas(&p->sbg, x + w - 1, y + h - 1, &cx1, &cy1);
    int X = cx0 < cx1 ? cx0 : cx1, Y = cy0 < cy1 ? cy0 : cy1;
    lv_obj_set_pos(l, vt_sx(a, X), vt_sy(a, Y));
    lv_obj_set_size(l, w * a->r->scale, h * a->r->scale);
    lv_obj_set_style_text_align(l, al, 0);
    if (p->L.flip) {
        lv_obj_set_style_transform_pivot_x(l, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(l, lv_pct(50), 0);
        lv_obj_set_style_transform_rotation(l, 1800, 0);
    }
}

void vt_view_hud_build(vt_app_t *a)
{
    if (a->hud) lv_obj_delete(a->hud);
    a->hud = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->hud);
    lv_obj_set_size(a->hud, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_SCROLLABLE);
    const uint32_t GOLD = 0xD9B45A, INK = 0xF4EEDC;
    for (int i = 0; i < 2; i++) {
        vt_player_t *p = &a->p[i];
        p->lb_score = p->lb_level = p->lb_lines = p->lb_cap_level = p->lb_cap_lines = NULL;
        p->lb_cap_next = p->lb_cap_hold = p->lb_pop = p->lb_info = NULL;
        p->shown_score = 0xFFFFFFFF;
        p->shown_level = p->shown_lines = p->shown_info = -1;
    }
    if (a->screen != SCR_GAME) return;
    for (int i = 0; i < a->nplayers; i++) {
        vt_player_t *p = &a->p[i];
        const vt_layout_t *L = &p->L;
        int w = L->cell * p->g.cols;
        bool small = a->land || a->nplayers == 2;
        const lv_font_t *fnum = small ? aos_font_body : aos_font_title;
        const lv_font_t *fcap = aos_font_tiny;

        /* the score in the arch */
        p->lb_score = label(a->hud, fnum, INK);
        int sh = small ? 18 : 24;
        place(a, p, p->lb_score, L->wx, L->wy - L->arch / 2 - sh / 2 - 2, w, sh, LV_TEXT_ALIGN_CENTER);

        /* where the level and the rows go */
        int bx, by, bw;
        if (a->nplayers == 2) {
            bx = L->nx - 4;
            by = L->ny + L->nn * L->nstep + 4;
            bw = L->aw - bx - 2;
        } else if (!a->land) {
            bx = 2;
            by = L->hy + L->nbox + 22;
            bw = L->wx - 7;
            /* the puzzle's buttons take the held piece's place and more */
            if (p->g.mode == VT_MODE_PUZZLE) by = L->hy + 160;
        } else {
            bx = 8;
            by = L->wy + 10;
            bw = L->hx - 22;
            if (p->g.mode == VT_MODE_PUZZLE) bw = L->wx - 30;
        }
        int capw = 9, numh = small ? 15 : 19;
        p->lb_cap_level = label(a->hud, fcap, GOLD);
        lv_label_set_text(p->lb_cap_level, p->g.mode == VT_MODE_PUZZLE ? _("PIEZAS") : _("NIVEL"));
        place(a, p, p->lb_cap_level, bx, by, bw, capw, LV_TEXT_ALIGN_CENTER);
        p->lb_level = label(a->hud, fnum, INK);
        place(a, p, p->lb_level, bx, by + capw + 1, bw, numh, LV_TEXT_ALIGN_CENTER);
        by += capw + numh + 8;
        p->lb_cap_lines = label(a->hud, fcap, GOLD);
        lv_label_set_text(p->lb_cap_lines, p->g.mode == VT_MODE_PUZZLE ? _("TIEMPO") : _("FILAS"));
        place(a, p, p->lb_cap_lines, bx, by, bw, capw, LV_TEXT_ALIGN_CENTER);
        p->lb_lines = label(a->hud, fnum, INK);
        place(a, p, p->lb_lines, bx, by + capw + 1, bw, numh, LV_TEXT_ALIGN_CENTER);
        by += capw + numh + 8;
        if (p->g.mode == VT_MODE_TIME) {
            lv_obj_t *cap = label(a->hud, fcap, GOLD);
            lv_label_set_text(cap, _("QUEDA"));
            place(a, p, cap, bx, by, bw, capw, LV_TEXT_ALIGN_CENTER);
            p->lb_info = label(a->hud, fnum, INK);
            place(a, p, p->lb_info, bx, by + capw + 1, bw, numh, LV_TEXT_ALIGN_CENTER);
        }
        if (p->g.mode == VT_MODE_PUZZLE) {
            /* the picture's name in the arch, where the score goes */
            char b[48];
            snprintf(b, sizeof b, "%d. %s", p->g.pz_level + 1, _(vt_pz_name(p->g.pz_level)));
            lv_obj_set_style_text_font(p->lb_score, small ? aos_font_small : aos_font_body, 0);
            lv_obj_set_style_text_color(p->lb_score, lv_color_hex(0xF2D58A), 0);
            lv_label_set_text(p->lb_score, b);
        }

        /* captions over the boxes */
        p->lb_cap_next = label(a->hud, fcap, GOLD);
        lv_label_set_text(p->lb_cap_next, _("SIGUE"));
        place(a, p, p->lb_cap_next, L->nx - 8, L->ny - 13, L->nbox + 16, capw, LV_TEXT_ALIGN_CENTER);
        if (L->hx >= 0 && p->g.mode != VT_MODE_PUZZLE) {
            p->lb_cap_hold = label(a->hud, fcap, GOLD);
            lv_label_set_text(p->lb_cap_hold, _("GUARDADA"));
            place(a, p, p->lb_cap_hold, L->hx - 8, L->hy - 13, L->nbox + 16, capw, LV_TEXT_ALIGN_CENTER);
        }

        /* the points of a clear, floating up the middle of the well */
        p->lb_pop = label(a->hud, small ? aos_font_body : aos_font_title, 0xFFFFFF);
        place(a, p, p->lb_pop, L->wx - 20, L->wy + L->cell * 8, w + 40, sh, LV_TEXT_ALIGN_CENTER);
        lv_obj_add_flag(p->lb_pop, LV_OBJ_FLAG_HIDDEN);
    }
    vt_view_hud(a);
}

static void fmt_num(char *b, size_t n, uint32_t v)
{
    /* thousands apart with a thin space, like 12 480 */
    if (v >= 1000000) snprintf(b, n, "%u %03u %03u", (unsigned)(v / 1000000), (unsigned)(v / 1000 % 1000), (unsigned)(v % 1000));
    else if (v >= 1000) snprintf(b, n, "%u %03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
    else snprintf(b, n, "%u", (unsigned)v);
}

void vt_view_hud(vt_app_t *a)
{
    if (a->screen != SCR_GAME) return;
    char b[48];
    for (int i = 0; i < a->nplayers; i++) {
        vt_player_t *p = &a->p[i];
        vt_game_t *g = &p->g;
        if (!p->lb_score) continue;
        if (g->mode == VT_MODE_PUZZLE) {
            int left = g->pz_n - g->pz_i;
            if (left != p->shown_level) {
                p->shown_level = left;
                snprintf(b, sizeof b, "%d", left);
                lv_label_set_text(p->lb_level, b);
            }
            int secs = (int)(g->time_ms / 1000);
            if (secs != p->shown_lines) {
                p->shown_lines = secs;
                snprintf(b, sizeof b, "%d:%02d", secs / 60, secs % 60);
                lv_label_set_text(p->lb_lines, b);
            }

        } else {
            if (g->score != p->shown_score) {
                p->shown_score = g->score;
                fmt_num(b, sizeof b, g->score);
                lv_label_set_text(p->lb_score, b);
            }
            if (g->level != p->shown_level) {
                p->shown_level = g->level;
                snprintf(b, sizeof b, "%d", g->level);
                lv_label_set_text(p->lb_level, b);
            }
            if (g->lines != p->shown_lines) {
                p->shown_lines = g->lines;
                snprintf(b, sizeof b, "%d", g->lines);
                lv_label_set_text(p->lb_lines, b);
            }
            if (g->mode == VT_MODE_TIME && p->lb_info) {
                int left = (int)((g->time_limit - g->time_ms + 999) / 1000);
                if (left != p->shown_info) {
                    p->shown_info = left;
                    snprintf(b, sizeof b, "%d:%02d", left / 60, left % 60);
                    lv_label_set_text(p->lb_info, b);
                    lv_obj_set_style_text_color(p->lb_info, lv_color_hex(left <= 10 ? 0xFF6A50 : 0xF4EEDC), 0);
                }
            }
        }
        /* the points float up and fade */
        if (p->lb_pop && !lv_obj_has_flag(p->lb_pop, LV_OBJ_FLAG_HIDDEN)) {
            int32_t left = (int32_t)(p->pop_until - a->now);
            if (left <= 0) {
                lv_obj_add_flag(p->lb_pop, LV_OBJ_FLAG_HIDDEN);
            } else {
                int k = 900 - left;
                lv_obj_set_style_translate_y(p->lb_pop, (p->L.flip ? 1 : -1) * k / 8, 0);
                lv_obj_set_style_opa(p->lb_pop, (lv_opa_t)(left > 300 ? 255 : left * 255 / 300), 0);
            }
        }
    }
}
