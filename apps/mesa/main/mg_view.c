/*
 * Juegos de mesa - the table on the screen.
 *
 * The board's picture was rendered by a fixed orthographic camera, so a
 * point of the board (u, v) in board units, h pixels up, lands on the
 * picture at
 *
 *     x = cx + S u'          y = cy - S sin(E) v' - h
 *
 * with (u', v') the point from the board's middle, turned as the board is
 * on the picture (rot quarter turns), S the pixels a unit and E the
 * camera's height over the table. One sprite of a piece serves every cell,
 * anchored at its foot. The flat view is the same with E = 90; a standing
 * board (four in a row) puts v up the screen. Passing the board turns it
 * half round: the picture is the same (the boards that may turn look the
 * same both ways), only where the cells are changes.
 *
 * The pieces are kept here by id. When the game's position changes, each
 * piece whose place changed flies from where it is drawn to where it now
 * stands, in an arc with its shadow on the table under it; a piece taken
 * leaves for its tray as the one that took it lands; a piece whose kind
 * changed in place (a disc turned, a pawn promoted) hops and shows the
 * new kind at the top of the hop.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mg_app.h"

#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define V (&mg->view)
#define P (&mg->play)

static float s_sin, s_cos;

static bool upright(void)
{
    return P->g && (P->g->flags & MESA_GF_UPRIGHT) && !mg->art.flat;
}

void mg_view_board_xy(float u, float v, float z, float *x, float *y)
{
    float du = u - V->bw * 0.5f, dv = v - V->bh * 0.5f, a, b;
    switch (V->rot & 3) {
    case 1: a = -dv; b = du; break;
    case 2: a = -du; b = -dv; break;
    case 3: a = dv; b = -du; break;
    default: a = du; b = dv; break;
    }
    const mg_geo_t *g = &V->geo;
    *x = V->ox + g->cx + g->S * a;
    if (upright()) *y = V->oy + g->cy - g->S * b - z;
    else *y = V->oy + g->cy - g->S * s_sin * b - z;
}

static bool screen_to_board(float x, float y, float *u, float *v)
{
    const mg_geo_t *g = &V->geo;
    if (g->S <= 0) return false;
    float a = (x - V->ox - g->cx) / g->S;
    float b = upright() ? -(y - V->oy - g->cy) / g->S : -(y - V->oy - g->cy) / (g->S * (s_sin > 0.05f ? s_sin : 0.05f));
    float du, dv;
    switch (V->rot & 3) {
    case 1: du = b; dv = -a; break;
    case 2: du = -a; dv = -b; break;
    case 3: du = -b; dv = a; break;
    default: du = a; dv = b; break;
    }
    *u = du + V->bw * 0.5f;
    *v = dv + V->bh * 0.5f;
    return true;
}

void mg_view_cell_xy(int cell, int slot, float *x, float *y)
{
    float u = 0, v = 0;
    P->g->cell_pos(P->st, cell, slot, &u, &v);
    mg_view_board_xy(u, v, 0, x, y);
}

/* a tray's slot on the screen */
static void tray_xy(int tray, int slot, float *x, float *y)
{
    /* the bottom player's tray is the near one; rows go away from the board's middle */
    bool near = tray == mg->play.bottom;
    const float *tr = V->tray[near ? 0 : 1];
    int per = (int)tr[2];
    if (per < 1) per = 1;
    int row = slot / per, col = slot % per;
    float S = V->geo.S;
    if (mg->portrait) {
        *x = tr[0] + (float)col * S * 0.5f;
        *y = tr[1] + (float)row * S * 0.32f * (near ? 1.0f : -1.0f);
    } else {
        *x = tr[0] + (float)col * S * 0.55f;
        *y = tr[1] + (float)row * S * 0.42f * (near ? 1.0f : -1.0f);
    }
}

static void piece_target(const mesa_piece_t *p, float *x, float *y)
{
    if (p->cell == MESA_NO_CELL) tray_xy(p->tray, p->slot, x, y);
    else mg_view_cell_xy(p->cell, p->slot, x, y);
}

bool mg_view_open(void)
{
    mg_view_t *v = V;
    size_t px = (size_t)720 * 1280;
    v->bg = (uint16_t *)mg_malloc(px * 2);
    v->fb = (uint16_t *)mg_malloc(px * 2);
    v->shown = (uint32_t *)mg_calloc(sizeof(uint32_t) * MG_MAX_TILES);
    if (!v->bg || !v->fb || !v->shown || !mg_gfx_init(&v->gfx)) {
        mg_view_close();
        return false;
    }
    v->cursor = -1;
    return true;
}

void mg_view_close(void)
{
    mg_view_t *v = V;
    if (v->canvas) lv_obj_delete(v->canvas);
    v->canvas = NULL;
    mg_free(v->bg);
    mg_free(v->fb);
    mg_free(v->shown);
    mg_gfx_free(&v->gfx);
    v->bg = v->fb = NULL;
    v->shown = NULL;
}

static void felt(uint16_t *dst, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) dst[i] = c;
}

/* the table under everything: the picture in the middle of the screen */
static void compose_bg(void)
{
    mg_view_t *v = V;
    const mg_art_t *a = &mg->art;
    uint16_t edge = mg_rgb(0x3A2614);
    if (!a->have_pic) {
        for (int y = 0; y < v->H; y++) felt(v->bg + (size_t)y * v->W, v->W, edge);
        return;
    }
    int pw = a->geo.pw, ph = a->geo.ph;
    /* the colour of the picture's corners for whatever it does not cover */
    edge = a->pic[0];
    int ox = (int)v->ox, oy = (int)v->oy;
    for (int y = 0; y < v->H; y++) {
        uint16_t *d = v->bg + (size_t)y * v->W;
        int py = y - oy;
        if (py < 0 || py >= ph) {
            int cy = py < 0 ? 0 : ph - 1;
            const uint16_t *s = a->pic + (size_t)cy * pw;
            for (int x = 0; x < v->W; x++) {
                int pxx = x - ox;
                d[x] = s[pxx < 0 ? 0 : pxx >= pw ? pw - 1 : pxx];
            }
            continue;
        }
        const uint16_t *s = a->pic + (size_t)py * pw;
        int x0 = ox < 0 ? 0 : ox, x1 = ox + pw > v->W ? v->W : ox + pw;
        for (int x = 0; x < x0; x++) d[x] = s[0];
        if (x1 > x0) memcpy(d + x0, s + (x0 - ox), (size_t)(x1 - x0) * 2);
        for (int x = x1; x < v->W; x++) d[x] = s[pw - 1];
    }
}

void mg_view_layout(void)
{
    mg_view_t *v = V;
    v->W = mg->W;
    v->H = mg->H;
    v->geo = mg->art.geo;
    if (v->geo.S <= 0) v->geo.S = 80;
    if (!mg->art.have_pic) {
        /* no picture: the board drawn by hand in the middle */
        v->geo.pw = v->W;
        v->geo.ph = v->H;
        v->geo.cx = v->W * 0.5f;
        v->geo.cy = v->H * 0.5f;
    }
    float bw = 8, bh = 8;
    P->g->board(P->opts, &bw, &bh);
    v->bw = bw;
    v->bh = bh;
    int turn = (P->g->flags & MESA_GF_ROTATE) && P->bottom == 1 ? 2 : 0;
    v->rot = (v->geo.rot + turn) & 3;
    s_sin = sinf(v->geo.E * 3.14159265f / 180.0f);
    s_cos = cosf(v->geo.E * 3.14159265f / 180.0f);
    v->ox = (float)(v->W - v->geo.pw) * 0.5f;
    v->oy = (float)(v->H - v->geo.ph) * 0.5f;
    /* the trays: in front of the board and behind it upright, at its sides
     * lying down */
    float x0, y0, x1, y1, xa, ya, xb, yb;
    mg_view_board_xy(0, 0, 0, &x0, &y0);
    mg_view_board_xy(bw, bh, 0, &x1, &y1);
    mg_view_board_xy(0, bh, 0, &xa, &ya);
    mg_view_board_xy(bw, 0, 0, &xb, &yb);
    float left = fminf(fminf(x0, x1), fminf(xa, xb));
    float top = fminf(fminf(y0, y1), fminf(ya, yb)), bottom = fmaxf(fmaxf(y0, y1), fmaxf(ya, yb));
    float S = v->geo.S;
    if (mg->portrait) {
        int per = (int)((v->W - 80) / (S * 0.5f));
        v->tray[0][0] = 40 + S * 0.3f;
        v->tray[0][1] = bottom + S * 0.95f;
        v->tray[0][2] = (float)per;
        v->tray[1][0] = 40 + S * 0.3f;
        v->tray[1][1] = top - S * 0.7f;
        v->tray[1][2] = (float)per;
    } else {
        /* both in the left column, between the players' cards */
        float lx = left - S * 0.55f;
        int per = (int)((lx - 30) / (S * 0.55f)) + 1;
        if (per > 5) per = 5;
        if (per < 1) per = 1;
        v->tray[0][0] = lx - (float)(per - 1) * S * 0.55f;
        v->tray[0][1] = v->H * 0.5f + S * 1.2f;
        v->tray[0][2] = (float)per;
        v->tray[1][0] = lx - (float)(per - 1) * S * 0.55f;
        v->tray[1][1] = v->H * 0.5f - S * 0.15f;
        v->tray[1][2] = (float)per;
    }
    compose_bg();
    mg_gfx_size(&v->gfx, v->W, v->H, v->bg);
    v->full = true;
    if (!v->canvas) {
        v->canvas = lv_canvas_create(mg->root);
        lv_obj_remove_flag(v->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(v->canvas, 0, 0);
    }
    lv_obj_remove_flag(v->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(v->canvas, 0);
    if (v->canvas) {
        lv_canvas_set_buffer(v->canvas, v->fb, v->W, v->H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(v->canvas, v->W, v->H);
    }
    /* the pieces where they stand, no flight */
    for (int i = 0; i < MG_SPR_MAX; i++) v->pc[i].used = false;
    mg_view_sync(false);
}

static float ease(float t)
{
    return t < 0 ? 0 : t > 1 ? 1 : t * t * (3 - 2 * t);
}

static uint32_t speed_ms(uint32_t ms)
{
    int sp = mg->prefs.speed;
    return sp == 0 ? ms * 3 / 2 : sp == 2 ? ms * 3 / 5 : ms;
}

void mg_view_sync(bool animate)
{
    mg_view_t *v = V;
    if (!P->g || !P->st) return;
    mesa_piece_t ps[MESA_MAX_PIECES];
    int n = P->g->pieces(P->st, ps, MESA_MAX_PIECES);
    bool seen[MG_SPR_MAX];
    memset(seen, 0, sizeof seen);
    uint32_t now = mg_now();
    float S = v->geo.S;
    /* the last move's path, for pieces that walk */
    const mesa_move_t *last = P->n > 0 && !P->hist[P->n - 1].roll ? &P->hist[P->n - 1].m : NULL;
    uint32_t land = 0;              /* when the piece that moved lands */
    /* first the pieces that stay on the board, then the ones that leave */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const mesa_piece_t *p = &ps[i];
            if (p->id >= MG_SPR_MAX) continue;
            bool off = p->cell == MESA_NO_CELL;
            if ((pass == 0) == off) continue;
            mg_piece_t *s = &v->pc[p->id];
            seen[p->id] = true;
            float tx, ty;
            piece_target(p, &tx, &ty);
            bool was = s->used;
            bool moved = !was || fabsf(tx - s->tx) > 0.5f || fabsf(ty - s->ty) > 0.5f;
            bool rekind = was && s->kind != p->kind;
            s->flags = p->flags;
            s->cell = p->cell;
            s->slot = p->slot;
            s->tray = p->tray;
            if (!animate) {
                s->used = true;
                s->id = p->id;
                s->kind = s->show_kind = p->kind;
                s->x = s->tx = s->fx = tx;
                s->y = s->ty = s->fy = ty;
                s->z = 0;
                s->moving = false;
                continue;
            }
            if (!was) {
                /* a new piece drops in: down its path if the game gives one
                 * (four in a row), else from above */
                s->used = true;
                s->id = p->id;
                s->kind = s->show_kind = p->kind;
                s->tx = tx;
                s->ty = ty;
                s->npath = 0;
                if (last && P->g->path) {
                    s->npath = P->g->path(P->st, last, p->id, s->path, (int)sizeof s->path);
                }
                if (s->npath > 0) {
                    /* from a cell above, straight down (behind a standing
                     * board's frame) */
                    mg_view_cell_xy(s->path[0], 0, &s->fx, &s->fy);
                    s->fy -= S * 1.2f;
                    s->npath = 0;
                    s->arc = -1;
                } else {
                    s->fx = tx;
                    s->fy = ty;
                    s->arc = -S * 2.0f;     /* negative: a fall */
                }
                s->x = s->fx;
                s->y = s->fy;
                s->t0 = now;
                s->dur = speed_ms(upright() ? 380 : 300);
                s->moving = true;
                s->landed = false;
                if (now + s->dur > land) land = now + s->dur;
                continue;
            }
            if (moved) {
                s->fx = s->x;
                s->fy = s->y;
                s->tx = tx;
                s->ty = ty;
                float d = sqrtf((tx - s->fx) * (tx - s->fx) + (ty - s->fy) * (ty - s->fy));
                s->npath = 0;
                if (!off && last && P->g->path) {
                    s->npath = P->g->path(P->st, last, p->id, s->path, (int)sizeof s->path);
                }
                s->t0 = off && land > now ? land - speed_ms(60) : now;
                if (s->npath > 1) {
                    s->dur = speed_ms((uint32_t)(150 * s->npath));
                    s->arc = S * 0.35f;
                } else {
                    s->dur = speed_ms((uint32_t)(260 + d * 0.45f));
                    s->arc = S * (off ? 0.9f : 0.45f + d / (S * 8.0f));
                }
                s->moving = true;
                s->landed = false;
                s->kind = p->kind;
                if (!rekind) s->show_kind = p->kind;
                if (!off && s->t0 + s->dur > land) land = s->t0 + s->dur;
            } else if (rekind) {
                /* turned in place: a hop, the new face at its top; turned
                 * discs go one after another out from the move */
                s->fx = s->tx = tx;
                s->fy = s->ty = ty;
                s->npath = 0;
                float delay = 0;
                if (last && last->n) {
                    float lx, ly;
                    mg_view_cell_xy(last->step[last->n - 1], 0, &lx, &ly);
                    delay = sqrtf((lx - tx) * (lx - tx) + (ly - ty) * (ly - ty)) * 0.9f;
                }
                s->t0 = now + speed_ms((uint32_t)(150 + delay));
                s->dur = speed_ms(260);
                s->arc = S * 0.4f;
                s->kind = p->kind;
                s->moving = true;
                s->landed = false;
            }
        }
    }
    for (int i = 0; i < MG_SPR_MAX; i++) {
        if (v->pc[i].used && !seen[i]) v->pc[i].used = false;
    }
    if (animate) v->anim_until = land > now ? land + 200 : now + 200;
}

bool mg_view_busy(void)
{
    mg_view_t *v = V;
    for (int i = 0; i < MG_SPR_MAX; i++) {
        if (v->pc[i].used && v->pc[i].moving) return true;
    }
    return false;
}

static void step_piece(mg_piece_t *s, uint32_t now)
{
    if (!s->moving) return;
    if ((int32_t)(now - s->t0) < 0) return;
    float t = (float)(now - s->t0) / (float)(s->dur ? s->dur : 1);
    if (t >= 1) {
        s->x = s->tx;
        s->y = s->ty;
        s->z = 0;
        s->moving = false;
        s->show_kind = s->kind;
        if (!s->landed) {
            s->landed = true;
            if (s->cell != MESA_NO_CELL) mg_audio_play(SND_MOVE);
        }
        return;
    }
    if (s->npath > 1) {
        /* hop by hop along the cells */
        float f = t * (float)(s->npath);
        int k = (int)f;
        float lt = f - (float)k;
        float ax, ay, bx, by;
        if (k == 0) {
            ax = s->fx;
            ay = s->fy;
        } else {
            mg_view_cell_xy(s->path[k - 1], 0, &ax, &ay);
        }
        if (k >= s->npath - 1) {
            bx = s->tx;
            by = s->ty;
        } else {
            mg_view_cell_xy(s->path[k], 0, &bx, &by);
        }
        float e = ease(lt);
        s->x = ax + (bx - ax) * e;
        s->y = ay + (by - ay) * e;
        s->z = s->arc * sinf(lt * 3.14159265f);
        return;
    }
    if (s->arc < 0) {
        /* a fall: accelerates, a little bounce at the end */
        float fall = -s->arc;
        if (t < 0.8f) {
            float k = t / 0.8f;
            s->z = fall * (1 - k * k);
        } else {
            float k = (t - 0.8f) / 0.2f;
            s->z = fall * 0.06f * sinf(k * 3.14159265f);
        }
        if (s->fx != s->tx || s->fy != s->ty) {
            float e = t < 0.8f ? (t / 0.8f) * (t / 0.8f) : 1;
            s->x = s->fx + (s->tx - s->fx) * e;
            s->y = s->fy + (s->ty - s->fy) * e;
            s->z = 0;
        }
        return;
    }
    float e = ease(t);
    s->x = s->fx + (s->tx - s->fx) * e;
    s->y = s->fy + (s->ty - s->fy) * e;
    s->z = s->arc * sinf(t * 3.14159265f);
    if (t > 0.5f) s->show_kind = s->kind;
}

static int frame_of(const mg_piece_t *s, int kind)
{
    const mesa_kind_t *k = &P->g->kinds[kind];
    if (!(k->flags & MESA_KF_FACING) || mg->art.flat) return 0;
    float cx, cy;
    mg_view_board_xy(V->bw * 0.5f, V->bh * 0.5f, 0, &cx, &cy);
    return s->x < cx ? 0 : 1;
}

static void draw_piece(const mg_piece_t *s, bool lifted)
{
    mg_gfx_t *g = &V->gfx;
    float S = V->geo.S;
    int kind = s->show_kind < P->g->nkinds ? s->show_kind : 0;
    float z = s->z + (lifted ? S * 0.28f : 0);
    const mesa_kind_t *k = &P->g->kinds[kind];
    bool flat = mg->art.flat;
    /* the shadow on the table, smaller and fainter the higher it is */
    float hk = 1.0f / (1.0f + z / (S * 1.2f));
    if (!upright()) {
        float rx = S * (k->flags & MESA_KF_FLAT ? 0.42f : 0.34f) * (0.6f + 0.4f * hk);
        float ry = flat ? rx : rx * (s_sin > 0.3f ? s_sin : 0.3f);
        float sx = s->x + (flat ? z * 0.25f : 0), sy = s->y + (flat ? z * 0.4f : S * 0.04f);
        mg_shadow(g, (int)sx, (int)sy, (int)rx, (int)ry, (int)(150 * hk));
    }
    int alpha = s->flags & MESA_PF_DIM ? 140 : 255;
    float y = s->y - (flat ? z * 0.15f : z);
    float x = s->x - (flat ? z * 0.1f : 0);
    if (mg->art.kind[kind].ok) {
        mg_spr(g, &mg->art.kind[kind].sheet, frame_of(s, kind), (int)x, (int)y, 0, alpha);
        return;
    }
    /* a stand-in: a disc, or a little column for the standing pieces */
    float r = S * (k->flags & MESA_KF_FLAT ? 0.4f : 0.32f);
    float ry = flat || upright() ? r : r * s_sin;
    uint16_t c = mg_rgb(k->color), c2 = mg_rgb(k->color2);
    if ((k->flags & MESA_KF_TALL) && !flat && !upright()) {
        float h = (float)k->height / 10.0f * S * s_cos * 0.8f;
        mg_disc(g, (int)x, (int)y, (int)r, (int)ry, c2, alpha);
        mg_rect(g, (int)(x - r), (int)(y - h), (int)(2 * r), (int)h, c, alpha, 0);
        mg_disc(g, (int)x, (int)(y - h), (int)r, (int)ry, c, alpha);
        mg_ring(g, (int)x, (int)(y - h), (int)r, (int)ry, 2, c2, alpha, 0);
        return;
    }
    float th = flat || upright() ? 0 : S * 0.12f * s_cos;
    if (th > 1) mg_disc(g, (int)x, (int)y, (int)r, (int)ry, c2, alpha);
    mg_disc(g, (int)x, (int)(y - th), (int)r, (int)ry, c, alpha);
    mg_ring(g, (int)x, (int)(y - th), (int)(r * 0.7f), (int)(ry * 0.7f), 3, c2, alpha, 0);
}

/* a cell's square on the screen, for the grids */
static void cell_quad(int cell, int16_t *q)
{
    float u, v;
    P->g->cell_pos(P->st, cell, 0, &u, &v);
    static const float D[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
    for (int i = 0; i < 4; i++) {
        float x, y;
        mg_view_board_xy(u + D[i][0], v + D[i][1], 0, &x, &y);
        q[2 * i] = (int16_t)x;
        q[2 * i + 1] = (int16_t)y;
    }
}

static void mark(int cell, uint32_t rgb, int a, bool full)
{
    if (cell == MESA_NO_CELL) return;
    float S = V->geo.S;
    if ((P->g->flags & MESA_GF_GRID) && full) {
        int16_t q[8];
        cell_quad(cell, q);
        mg_quad(&V->gfx, q, mg_rgb(rgb), a);
        return;
    }
    float x, y;
    mg_view_cell_xy(cell, 0, &x, &y);
    float r = S * 0.42f, ry = mg->art.flat || upright() ? r : r * s_sin;
    mg_disc(&V->gfx, (int)x, (int)y, (int)r, (int)ry, mg_rgb(rgb), a);
}

static bool occupied(int cell)
{
    for (int i = 0; i < MG_SPR_MAX; i++) {
        if (V->pc[i].used && V->pc[i].cell == cell) return true;
    }
    return false;
}

static void marks(void)
{
    mg_gfx_t *g = &V->gfx;
    float S = V->geo.S;
    float rs = mg->art.flat || upright() ? 1.0f : s_sin;
    /* the last move */
    if (P->n > 0) {
        for (int k = P->n - 1; k >= 0 && k >= P->n - 2; k--) {
            const mg_hist_t *h = &P->hist[k];
            if (h->roll) continue;
            if (h->m.n) {
                mark(h->m.step[0], 0xE8C040, 70, true);
                mark(h->m.step[h->m.n - 1], 0xE8C040, 95, true);
            }
            break;
        }
    }
    /* a king in check */
    const char *st = P->g->status ? P->g->status(P->st) : NULL;
    if (st && P->g == &mesa_ajedrez && P->g->turn(P->st) >= 0) {
        for (int i = 0; i < MG_SPR_MAX; i++) {
            const mg_piece_t *s = &V->pc[i];
            if (!s->used || s->cell == MESA_NO_CELL) continue;
            if (s->kind == (P->g->turn(P->st) ? 11 : 5)) {
                float r = S * 0.55f;
                mg_disc(g, (int)s->x, (int)s->y, (int)r, (int)(r * rs), mg_rgb(0xFF3020), 110);
            }
        }
    }
    /* what is being moved, and where it may go */
    if (P->sel > 0) {
        for (int k = 0; k < P->sel; k++) mark(P->chosen[k], 0x58B8FF, 120, true);
        bool hint = mg->prefs.hints || !(P->g->flags & MESA_GF_HINTS);
        uint8_t done[256];
        memset(done, 0, sizeof done);
        for (int i = 0; i < P->nmoves && hint; i++) {
            const mesa_move_t *m = &P->moves[i];
            if (m->n <= P->sel || memcmp(m->step, P->chosen, (size_t)P->sel)) continue;
            int c = m->step[P->sel];
            if (done[c]) continue;
            done[c] = 1;
            float x, y;
            mg_view_cell_xy(c, 0, &x, &y);
            if (occupied(c)) {
                mg_ring(g, (int)x, (int)y, (int)(S * 0.44f), (int)(S * 0.44f * rs), (int)(S * 0.07f) + 1,
                        mg_rgb(0x30D158), 200, 0);
            } else {
                mg_disc(g, (int)x, (int)y, (int)(S * 0.15f), (int)(S * 0.15f * rs), mg_rgb(0x30D158), 190);
            }
        }
    } else if (mg->prefs.hints && P->nmoves && mg_play_person_to_move() && P->g->moves &&
               !(P->g->flags & MESA_GF_HINTS)) {
        /* games where a move is one tap (reversi, four in a row): where */
        uint8_t done[256];
        memset(done, 0, sizeof done);
        for (int i = 0; i < P->nmoves; i++) {
            const mesa_move_t *m = &P->moves[i];
            if (m->n != 1 || done[m->step[0]]) continue;
            done[m->step[0]] = 1;
            float x, y;
            mg_view_cell_xy(m->step[0], 0, &x, &y);
            mg_disc(g, (int)x, (int)y, (int)(S * 0.13f), (int)(S * 0.13f * rs), mg_rgb(0x30D158), 150);
        }
    }
    /* the gamepad's frame */
    if (V->cursor >= 0) {
        float x, y;
        mg_view_cell_xy(V->cursor, 0, &x, &y);
        if (P->g->flags & MESA_GF_GRID) {
            int16_t q[8];
            cell_quad(V->cursor, q);
            mg_beam(g, q[0], q[1], q[2], q[3], 2, mg_rgb(0xFFFFFF), 230);
            mg_beam(g, q[2], q[3], q[4], q[5], 2, mg_rgb(0xFFFFFF), 230);
            mg_beam(g, q[4], q[5], q[6], q[7], 2, mg_rgb(0xFFFFFF), 230);
            mg_beam(g, q[6], q[7], q[0], q[1], 2, mg_rgb(0xFFFFFF), 230);
        } else {
            mg_ring(g, (int)x, (int)y, (int)(S * 0.5f), (int)(S * 0.5f * rs), 4, mg_rgb(0xFFFFFF), 230, 0);
        }
    }
}

static void draw_dice(uint32_t now)
{
    if (!P->g->dice) return;
    int vals[4] = { 0 };
    bool used[4] = { false };
    int n = P->g->dice(P->st, vals, used);
    if (P->rolling) {
        n = P->g->need_roll ? P->g->need_roll(P->st) : 2;
        if (n < 1) n = 2;
        for (int i = 0; i < n && i < 4; i++) {
            vals[i] = P->roll_shown[i];
            used[i] = false;
        }
    }
    if (n <= 0) return;
    float u = V->bw * 0.5f, v = V->bh * 0.5f;
    if (P->g->dice_pos) P->g->dice_pos(P->st, &u, &v);
    float cx, cy;
    mg_view_board_xy(u, v, 0, &cx, &cy);
    float S = V->geo.S;
    float step = S * 1.05f;
    mg_gfx_t *g = &V->gfx;
    for (int i = 0; i < n && i < 4; i++) {
        float x = cx + ((float)i - (float)(n - 1) * 0.5f) * step;
        float y = cy;
        float z = 0;
        if (P->rolling) {
            float t = (float)(now - P->roll_t0) / 600.0f;
            z = fabsf(sinf((t * 2.5f + (float)i * 0.3f) * 3.14159265f)) * S * 0.6f * (1.0f - t);
            if (z < 0) z = 0;
        }
        int face = vals[i] >= 1 && vals[i] <= 6 ? vals[i] - 1 : 0;
        int a = used[i] ? 110 : 255;
        mg_shadow(g, (int)x, (int)(y + S * 0.05f), (int)(S * 0.42f), (int)(S * 0.42f * (mg->art.flat ? 1 : s_sin)), 120);
        if (mg->art.have_dice) {
            mg_spr(g, &mg->art.dice, face, (int)x, (int)(y - z), 0, a);
            continue;
        }
        /* a stand-in die: a white square and its pips */
        float h = S * 0.36f;
        mg_rect(g, (int)(x - h), (int)(y - z - 2 * h), (int)(2 * h), (int)(2 * h), mg_rgb(0xF4F0E6), a, (int)(h * 0.3f));
        static const int8_t PIPS[6][7][2] = {
            { { 0, 0 } }, { { -1, -1 }, { 1, 1 } }, { { -1, -1 }, { 0, 0 }, { 1, 1 } },
            { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } }, { { -1, -1 }, { 1, -1 }, { 0, 0 }, { -1, 1 }, { 1, 1 } },
            { { -1, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 } },
        };
        for (int k = 0; k <= face; k++) {
            int px = (int)(x + PIPS[face][k][0] * h * 0.55f), py = (int)(y - z - h + PIPS[face][k][1] * h * 0.55f);
            mg_disc(g, px, py, (int)(h * 0.18f) + 1, (int)(h * 0.18f) + 1, mg_rgb(0x201810), a);
        }
    }
}

void mg_view_redraw(void)
{
    V->full = true;
}

void mg_view_frame(void)
{
    mg_view_t *v = V;
    if (!v->canvas || !P->g) return;
    uint32_t now = mg_now();
    mg_gfx_t *g = &v->gfx;
    for (int i = 0; i < MG_SPR_MAX; i++) {
        if (v->pc[i].used) step_piece(&v->pc[i], now);
    }
    mg_gfx_begin(g);
    marks();
    /* back to front; what flies, last */
    int order[MG_SPR_MAX], n = 0;
    for (int i = 0; i < MG_SPR_MAX; i++) {
        if (v->pc[i].used) order[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        const mg_piece_t *a = &v->pc[k];
        float ka = a->y + (a->z > 1 ? 4000 : 0);
        while (j >= 0) {
            const mg_piece_t *b = &v->pc[order[j]];
            float kb = b->y + (b->z > 1 ? 4000 : 0);
            if (kb <= ka) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    int lift = P->sel > 0 ? P->chosen[0] : -1;
    /* with a standing board the frame goes over the discs, the dice over everything */
    for (int i = 0; i < n; i++) {
        const mg_piece_t *s = &v->pc[order[i]];
        draw_piece(s, s->cell == lift && !s->moving && lift >= 0 && !(P->g->flags & MESA_GF_UPRIGHT));
    }
    if (mg->art.have_over) {
        mg_spr(g, &mg->art.over, 0, (int)(v->ox + v->geo.cx), (int)(v->oy + v->geo.cy), 0, 255);
    }
    draw_dice(now);
    uint32_t t0 = (uint32_t)aos_hal_uptime_us();
    mg_gfx_end(g, v->fb, v->shown, v->full);
    uint32_t t1 = (uint32_t)aos_hal_uptime_us();
    v->full = false;
    if (g->dirty.x1 > g->dirty.x0) {
        lv_area_t a = { g->dirty.x0, g->dirty.y0, g->dirty.x1 - 1, g->dirty.y1 - 1 };
        lv_obj_invalidate_area(v->canvas, &a);
    }
    mg->perf_frames++;
    mg->perf_us += t1 - t0;
    mg->perf_tiles += (uint32_t)g->drawn_tiles;
}

int mg_view_hit(int x, int y)
{
    float u, v;
    if (!screen_to_board((float)x, (float)y, &u, &v)) return MESA_NO_CELL;
    return P->g->cell_at(P->st, u, v);
}

/* the piece drawn under a point, its cell (the tall ones stick up over the
 * cells behind them) */
int mg_view_hit_piece(int x, int y)
{
    mg_view_t *v = V;
    float S = v->geo.S;
    int best = MESA_NO_CELL;
    float best_y = -1e9f;
    for (int i = 0; i < MG_SPR_MAX; i++) {
        const mg_piece_t *s = &v->pc[i];
        if (!s->used || s->cell == MESA_NO_CELL) continue;
        int kind = s->show_kind;
        float x0, x1, y0, y1;
        if (mg->art.kind[kind].ok) {
            const mg_frame_t *fr = &mg->art.kind[kind].sheet.fr[frame_of(s, kind) % mg->art.kind[kind].sheet.nframes];
            x0 = s->x + fr->ox + fr->w * 0.15f;
            x1 = s->x + fr->ox + fr->w * 0.85f;
            y0 = s->y + fr->oy + fr->h * 0.05f;
            y1 = s->y + fr->oy + fr->h;
        } else {
            x0 = s->x - S * 0.35f;
            x1 = s->x + S * 0.35f;
            y0 = s->y - S * 0.9f;
            y1 = s->y + S * 0.2f;
        }
        if (x < x0 || x > x1 || y < y0 || y > y1) continue;
        /* the nearest (lowest on the screen) wins */
        if (s->y > best_y) {
            best_y = s->y;
            best = s->cell;
        }
    }
    return best;
}
