/*
 * SOLITARIO - the view: where every pile goes, the cards as objects, and
 * how they move.
 *
 * The felt is one opaque RGB565 canvas drawn once per size. Every card is an
 * lv_image whose source is one of 52 face images (or the back) drawn at the
 * size this layout wants: a hundred cards on the table are a hundred small
 * objects sharing a few images, so moving one repaints only where it was and
 * where it goes (APP-GUIDE 6.4).
 *
 * The motion is the app's own, as in Blackjack: the frame timer walks every
 * card along its tween, turns it by squeezing its width (with a slight
 * growth in height at the edge, which is what sells the third dimension),
 * pops it when it lands at home, and leans the cards a finger carries. No
 * lv_anim: nothing is left to call back into the app after it closes.
 *
 * After each move the game's log says which cards moved and in what order;
 * so_view_sync() sends each to its new place, later ones a little later, so
 * a deal is dealt and a redeal is turned over card by card.
 */
#include "so_app.h"

#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLIP_MS     90          /* each half of a turn                    */
#define POP_MS      240
#define RING_MS     520
#define MAX_CW      132

/* --- geometry ---------------------------------------------------------------- */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static void pl_set(so_pl_t *p, int x, int y, int slot)
{
    memset(p, 0, sizeof *p);
    p->x = (int16_t)x;
    p->y = (int16_t)y;
    p->slot = (uint8_t)slot;
}

/* a column fanned down to 'bottom' */
static void pl_col(so_view_t *v, so_pl_t *p, int x, int y, int bottom, int slot)
{
    pl_set(p, x, y, slot);
    p->dy = (int16_t)(v->art.strip + imax(2, v->cw / 40));
    p->ddy = (int16_t)imax(8, v->ch / 9);
    p->room = (int16_t)(bottom - y);
}

/* the card's size for this game, from the width it may take and the height */
static void card_size(so_app_t *a, int w_max, int h_max)
{
    so_view_t *v = &a->v;
    bool es = a->g.kind == SO_K_SPANISH;
    int cw = imin(w_max, MAX_CW);
    int ch = es ? so_es_height(cw) : so_card_h_for(cw);
    if (ch > h_max) {
        /* narrower until it fits */
        while (cw > 30 && ch > h_max) {
            cw--;
            ch = es ? so_es_height(cw) : so_card_h_for(cw);
        }
    }
    v->cw = cw;
    v->ch = ch;
}

void so_view_layout(so_app_t *a)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    int W = v->W, H = v->H;
    v->land = W > H;

    /* the play area: below the bar upright, right of the side panel lying down */
    if (v->land) {
        v->play = (lv_area_t){ 214, SO_EDGE_TOP, W - 14, H - 24 };
    } else {
        v->play = (lv_area_t){ 12, SO_EDGE_TOP + 84, W - 12, H - SO_EDGE_BOT - 116 };
    }
    int x0 = v->play.x1, y0 = v->play.y1, x1 = v->play.x2, y1 = v->play.y2;
    int PW = x1 - x0, PH = y1 - y0;
    bool land = v->land;
    memset(v->pl, 0, sizeof v->pl);

    switch (g->kind) {
    case SO_K_KLONDIKE:
    case SO_K_SPANISH:
    case SO_K_YUKON: {
        bool stock = g->kind != SO_K_YUKON;
        if (!land) {
            int gap = 8;
            card_size(a, (PW - 6 * gap) / 7, (PH - 30) / 4);
            int step = v->cw + gap;
            int ox = x0 + (PW - 7 * v->cw - 6 * gap) / 2;
            if (stock) {
                pl_set(&v->pl[g->stock], ox, y0, SO_SLOT_STOCK);
                pl_set(&v->pl[g->waste], ox + step, y0, 255);
                v->pl[g->waste].dx = (int16_t)(v->cw * 30 / 100);
                v->pl[g->waste].fan_last = (int8_t)(g->draw > 1 ? 3 : 1);
            }
            for (int i = 0; i < 4; i++) {
                pl_set(&v->pl[g->found0 + i], ox + (3 + i) * step, y0, SO_SLOT_HOME);
            }
            int ty = y0 + v->ch + 20;
            for (int i = 0; i < 7; i++) {
                pl_col(v, &v->pl[g->tab0 + i], ox + i * step, ty, y1,
                       g->kind == SO_K_SPANISH ? SO_SLOT_PLAIN : SO_SLOT_KING);
            }
        } else {
            int gap = 12, cols = stock ? 9 : 8;
            card_size(a, (PW - (cols - 1) * gap - 24) / cols, (PH - 3 * 10) / 4);
            int step = v->cw + gap;
            int ox = x0 + (PW - cols * v->cw - (cols - 1) * gap - 24) / 2;
            int tx = ox;
            if (stock) {
                pl_set(&v->pl[g->stock], ox, y0, SO_SLOT_STOCK);
                pl_set(&v->pl[g->waste], ox, y0 + v->ch + 16, 255);
                v->pl[g->waste].dy = (int16_t)(v->art.strip * 9 / 10);
                v->pl[g->waste].fan_last = (int8_t)(g->draw > 1 ? 3 : 1);
                tx = ox + step + 12;
            }
            for (int i = 0; i < 7; i++) {
                pl_col(v, &v->pl[g->tab0 + i], tx + i * step, y0, y1,
                       g->kind == SO_K_SPANISH ? SO_SLOT_PLAIN : SO_SLOT_KING);
            }
            int fx = tx + 7 * step + 12 - gap + gap;
            int fstep = imin(v->ch + 10, (PH - v->ch) / 3);
            for (int i = 0; i < 4; i++) {
                pl_set(&v->pl[g->found0 + i], fx, y0 + i * fstep, SO_SLOT_HOME);
            }
        }
        break;
    }
    case SO_K_FREECELL: {
        if (!land) {
            int gap = 6;
            card_size(a, (PW - 7 * gap) / 8, (PH - 30) / 4);
            int step = v->cw + gap;
            int ox = x0 + (PW - 8 * v->cw - 7 * gap) / 2;
            for (int i = 0; i < 4; i++) {
                pl_set(&v->pl[g->cell0 + i], ox + i * step, y0, SO_SLOT_PLAIN);
                pl_set(&v->pl[g->found0 + i], ox + (4 + i) * step, y0, SO_SLOT_HOME);
            }
            int ty = y0 + v->ch + 20;
            for (int i = 0; i < 8; i++) {
                pl_col(v, &v->pl[g->tab0 + i], ox + i * step, ty, y1, SO_SLOT_PLAIN);
            }
        } else {
            int gap = 10;
            card_size(a, (PW - 9 * gap - 24) / 10, (PH - 3 * 8) / 4);
            int step = v->cw + gap;
            int ox = x0 + (PW - 10 * v->cw - 9 * gap - 24) / 2;
            int fstep = imin(v->ch + 8, (PH - v->ch) / 3);
            for (int i = 0; i < 4; i++) {
                pl_set(&v->pl[g->cell0 + i], ox, y0 + i * fstep, SO_SLOT_PLAIN);
                pl_set(&v->pl[g->found0 + i], ox + 9 * step + 24, y0 + i * fstep, SO_SLOT_HOME);
            }
            for (int i = 0; i < 8; i++) {
                pl_col(v, &v->pl[g->tab0 + i], ox + step + 12 + i * step, y0, y1, SO_SLOT_PLAIN);
            }
        }
        break;
    }
    case SO_K_SPIDER: {
        if (!land) {
            int gap = 4;
            card_size(a, (PW - 9 * gap) / 10, (PH - 30) / 5);
            int step = v->cw + gap;
            int ox = x0 + (PW - 10 * v->cw - 9 * gap) / 2;
            for (int i = 0; i < 8; i++) {
                pl_set(&v->pl[g->found0 + i], ox + i * (v->cw * 36 / 100), y0, 255);
            }
            pl_set(&v->pl[g->stock], ox + 9 * step, y0, 255);
            v->pl[g->stock].ddx = (int16_t)-(v->cw * 26 / 100);
            int ty = y0 + v->ch + 16;
            for (int i = 0; i < 10; i++) {
                pl_col(v, &v->pl[g->tab0 + i], ox + i * step, ty, y1, SO_SLOT_PLAIN);
            }
        } else {
            int gap = 8;
            card_size(a, (PW - 10 * gap - 16) / 11, PH / 3);
            int step = v->cw + gap;
            int ox = x0 + (PW - 11 * v->cw - 10 * gap - 16) / 2;
            for (int i = 0; i < 10; i++) {
                pl_col(v, &v->pl[g->tab0 + i], ox + i * step, y0, y1, SO_SLOT_PLAIN);
            }
            int sx = ox + 10 * step + 16;
            for (int i = 0; i < 8; i++) {
                pl_set(&v->pl[g->found0 + i], sx, y0 + i * (v->ch * 20 / 100), 255);
            }
            pl_set(&v->pl[g->stock], sx, y1 - v->ch, 255);
            v->pl[g->stock].ddy = (int16_t)-(v->ch * 14 / 100);
        }
        break;
    }
    case SO_K_PYRAMID: {
        int gap = 6;
        int rows_h, side;
        if (!land) {
            card_size(a, (PW - 6 * gap) / 7, (PH - 60) * 100 / 452);
            side = 0;
        } else {
            card_size(a, (PW - 6 * gap - 40) * 100 / 800, PH * 100 / 352);
            side = v->cw + 40;
        }
        int step = v->cw + gap, rstep = v->ch * 42 / 100;
        rows_h = v->ch + 6 * rstep;
        int pw = 7 * v->cw + 6 * gap;
        int ox = x0 + (PW - pw - side) / 2;
        int below = land ? 0 : v->ch + 36;
        int oy = y0 + imax(0, (PH - rows_h - below) / 2);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c <= r; c++) {
                int s = g->slot0 + r * (r + 1) / 2 + c;
                int x = ox + (pw - (r + 1) * v->cw - r * gap) / 2 + c * step;
                pl_set(&v->pl[s], x, oy + r * rstep, 255);
            }
        }
        if (!land) {
            int sy = oy + rows_h + 36;
            int cx = x0 + PW / 2;
            pl_set(&v->pl[g->stock], cx - v->cw - step, sy, SO_SLOT_STOCK);
            pl_set(&v->pl[g->waste], cx - v->cw / 2, sy, 255);
            pl_set(&v->pl[g->discard], cx + step, sy, 255);
        } else {
            int sx = ox + pw + 40;
            pl_set(&v->pl[g->stock], sx, y0, SO_SLOT_STOCK);
            pl_set(&v->pl[g->waste], sx, y0 + v->ch + 20, 255);
            pl_set(&v->pl[g->discard], sx, y1 - v->ch, 255);
        }
        v->pl[g->discard].pad_skip = true;
        break;
    }
    case SO_K_TRIPEAKS: {
        int gap = land ? 6 : 3;
        card_size(a, (PW - 9 * gap) / 10, (PH - 60) * 100 / 400);
        int step = v->cw + gap, rstep = v->ch * 46 / 100;
        int pw = 10 * v->cw + 9 * gap;
        int ox = x0 + (PW - pw) / 2;
        int peaks_h = v->ch + 3 * rstep;
        int oy = y0 + imax(0, (PH - peaks_h - v->ch - 44) / 2);
        int s0 = g->slot0;
        for (int p = 0; p < 3; p++) {
            pl_set(&v->pl[s0 + p], ox + (3 * p + 1) * step + step / 2, oy, 255);
        }
        for (int q = 0; q < 6; q++) {
            int p = q / 2;
            pl_set(&v->pl[s0 + 3 + q], ox + (3 * p + (q & 1) + 1) * step, oy + rstep, 255);
        }
        for (int j = 0; j < 9; j++) {
            pl_set(&v->pl[s0 + 9 + j], ox + j * step + step / 2, oy + 2 * rstep, 255);
        }
        for (int j = 0; j < 10; j++) {
            pl_set(&v->pl[s0 + 18 + j], ox + j * step, oy + 3 * rstep, 255);
        }
        int sy = oy + peaks_h + 44;
        int cx = x0 + PW / 2;
        pl_set(&v->pl[g->stock], cx - v->cw - 30, sy, SO_SLOT_DONE);
        v->pl[g->stock].ddx = (int16_t)-imax(1, v->cw / 40);
        pl_set(&v->pl[g->waste], cx + 30, sy, 255);
        break;
    }
    case SO_K_GOLF: {
        int gap = land ? 12 : 8;
        int strip0 = a->v.art.strip;
        (void)strip0;
        if (!land) {
            card_size(a, (PW - 6 * gap) / 7, (PH - 60) * 100 / 330);
        } else {
            card_size(a, (PW - 6 * gap) / 7, (PH - 30) * 100 / 300);
        }
        int step = v->cw + gap;
        int ox = x0 + (PW - 7 * v->cw - 6 * gap) / 2;
        /* five cards fanned: the strip is a third of the width or so */
        int fan = v->cw * 38 / 100;
        int colh = v->ch + 4 * fan;
        int oy = y0 + imax(0, (PH - colh - v->ch - 40) / 2);
        for (int i = 0; i < 7; i++) {
            pl_col(v, &v->pl[g->tab0 + i], ox + i * step, oy, oy + colh, 255);
        }
        int sy = oy + colh + 40;
        pl_set(&v->pl[g->stock], ox, sy, SO_SLOT_DONE);
        v->pl[g->stock].ddx = (int16_t)imax(1, v->cw / 30);
        pl_set(&v->pl[g->waste], ox + step + gap, sy, 255);
        v->pl[g->waste].dx = (int16_t)(v->cw * 30 / 100);
        v->pl[g->waste].fan_last = 5;
        break;
    }
    }
}

/* --- where a card goes ------------------------------------------------------------- */

void so_view_card_pos(so_app_t *a, int p, int idx, float *ox, float *oy)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    const so_pl_t *pl = &v->pl[p];
    float x = pl->x, y = pl->y;
    int   n = g->pn[p];
    int   t = g->ptype[p];

    if (t == SO_P_TABLEAU) {
        /* down the column, squeezed if it would run past its room */
        int nd = 0, nu = 0;
        for (int i = 0; i < n - 1; i++) {
            if (g->up[g->pc[p][i]]) nu++;
            else nd++;
        }
        float dyu = pl->dy, dyd = pl->ddy;
        float need = (float)nu * dyu + (float)nd * dyd + (float)v->ch;
        if (need > (float)pl->room && nu > 0) {
            float minu = (float)v->art.strip * 0.42f;
            float left = (float)pl->room - (float)v->ch - (float)nd * dyd;
            dyu = left / (float)nu;
            if (dyu < minu) {
                dyu = minu;
                float l2 = (float)pl->room - (float)v->ch - (float)nu * dyu;
                dyd = nd ? l2 / (float)nd : dyd;
                if (dyd < 3.f) dyd = 3.f;
            }
        }
        for (int i = 0; i < idx && i < n; i++) {
            y += g->up[g->pc[p][i]] ? dyu : dyd;
        }
    } else if (pl->fan_last > 0) {
        int first = n - pl->fan_last;
        if (first < 0) first = 0;
        int k = idx - first;
        if (k > 0) {
            x += (float)(k * pl->dx);
            y += (float)(k * pl->dy);
        }
    } else if (t == SO_P_STOCK && g->kind == SO_K_SPIDER) {
        int grp = idx / 10;
        x += (float)(grp * pl->ddx);
        y += (float)(grp * pl->ddy);
    } else if (t == SO_P_STOCK) {
        int grp = idx / 4;
        x += (float)(grp * pl->ddx);
        y += (float)(grp * pl->ddy);
    } else if (t == SO_P_FOUND || t == SO_P_DISCARD || t == SO_P_STOCK) {
        /* a hint of thickness */
        x -= (float)(idx / 6);
        y -= (float)(idx / 6);
    }
    *ox = x;
    *oy = y;
}

/* --- images ------------------------------------------------------------------------ */

static void dsc_of(lv_image_dsc_t *d, const so_img_t *img)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = (uint32_t)img->w;
    d->header.h = (uint32_t)img->h;
    d->header.stride = (uint32_t)img->w * 4;
    d->data = (const uint8_t *)img->px;
    d->data_size = (uint32_t)img->w * img->h * 4;
}

static int face_key(so_app_t *a, int card)
{
    so_game_t *g = &a->g;
    return a->v.deck_kind ? g->suit[card] * 10 + g->rank[card] : g->suit[card] * 13 + g->rank[card];
}

lv_image_dsc_t *so_view_face(so_app_t *a, int card)
{
    return &a->v.face_d[face_key(a, card)];
}

so_img_t *so_view_face_img(so_app_t *a, int card)
{
    return &a->v.face[face_key(a, card)];
}

static void images_free(so_view_t *v)
{
    for (int i = 0; i < 52; i++) {
        if (v->face[i].px) lv_image_cache_drop(&v->face_d[i]);
        so_img_free(&v->face[i]);
    }
    if (v->back.px) lv_image_cache_drop(&v->back_d);
    so_img_free(&v->back);
    for (int i = 0; i < SO_NSLOTS; i++) {
        if (v->slot[i].px) lv_image_cache_drop(&v->slot_d[i]);
        so_img_free(&v->slot[i]);
    }
    v->art_cw = 0;
}

/* Every image at the layout's size; nothing is drawn again if the size and
 * the deck are what they were. */
static bool images_make(so_app_t *a)
{
    so_view_t *v = &a->v;
    int deck_kind = a->g.kind == SO_K_SPANISH ? 1 : 0;
    if (v->art_cw == v->cw && v->art_ch == v->ch && v->art_deck == a->set.deck &&
        v->deck_kind == deck_kind && v->face[0].px) {
        return true;
    }
    uint64_t t0 = aos_hal_uptime_ms();
    images_free(v);
    v->deck_kind = deck_kind;
    so_art_setup(&v->art, v->cw, v->ch, deck_kind ? SO_DECK_CLASSIC : a->set.deck, 0);
    if (deck_kind) {
        /* the Spanish deck's strip: its number is larger than ours */
        v->art.strip = v->cw * 40 / 100;
    }
    int nf = deck_kind ? 40 : 52;
    for (int i = 0; i < nf; i++) {
        v->face[i] = so_img_new(v->art.cv_w, v->art.cv_h);
        if (!v->face[i].px) {
            return false;
        }
        if (deck_kind) {
            so_es_render(a->root, &v->face[i], v->cw, v->ch, v->art.pad, i / 10, i % 10);
        } else {
            so_card_draw(&v->art, &v->face[i], i / 13, i % 13);
        }
        dsc_of(&v->face_d[i], &v->face[i]);
    }
    v->back = so_img_new(v->art.cv_w, v->art.cv_h);
    for (int i = 0; i < SO_NSLOTS; i++) {
        v->slot[i] = so_img_new(v->art.cv_w, v->art.cv_h);
    }
    if (!v->back.px) {
        return false;
    }
    so_back_draw(&v->art, &v->back, a->set.back);
    v->back_id = a->set.back;
    dsc_of(&v->back_d, &v->back);
    for (int i = 0; i < SO_NSLOTS; i++) {
        if (!v->slot[i].px) return false;
        so_slot_draw(&v->art, &v->slot[i], i);
        dsc_of(&v->slot_d[i], &v->slot[i]);
    }
    v->art_cw = v->cw;
    v->art_ch = v->ch;
    v->art_deck = a->set.deck;
    aos_hal_log(SO_TAG, "cards %dx%d drawn in %u ms", v->cw, v->ch,
                (unsigned)(aos_hal_uptime_ms() - t0));
    return true;
}

/* --- building ------------------------------------------------------------------------ */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *outline(lv_obj_t *parent, uint32_t color, int bw)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static bool felt_make(so_app_t *a)
{
    so_view_t *v = &a->v;
    size_t n = (size_t)v->W * v->H;
    v->feltmem = (uint16_t *)malloc(n * 2);
    v->feltclean = (uint16_t *)malloc(n * 2);
    if (!v->feltmem || !v->feltclean) {
        return false;
    }
    uint64_t t0 = aos_hal_uptime_ms();
    so_felt_draw565(v->feltclean, v->W, v->H, a->set.felt);
    memcpy(v->feltmem, v->feltclean, n * 2);
    v->felt_id = a->set.felt;
    aos_hal_log(SO_TAG, "felt %d %dx%d drawn in %u ms", a->set.felt, v->W, v->H,
                (unsigned)(aos_hal_uptime_ms() - t0));
    return true;
}

bool so_view_build(so_app_t *a)
{
    so_view_t *v = &a->v;
    so_view_layout(a);
    if (!images_make(a)) {
        aos_hal_log(SO_TAG, "out of memory for the cards");
        return false;
    }
    /* the columns' fans come from the art's strip: lay out again with it */
    so_view_layout(a);
    if (!v->feltmem && !felt_make(a)) {
        aos_hal_log(SO_TAG, "out of memory for the felt");
        return false;
    }

    v->felt = lv_canvas_create(a->root);
    lv_canvas_set_buffer(v->felt, v->feltmem, v->W, v->H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(v->felt, v->W, v->H);
    lv_obj_set_pos(v->felt, 0, 0);
    lv_obj_remove_flag(v->felt, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *slots = plain(a->root);
    lv_obj_set_size(slots, v->W, v->H);
    for (int p = 0; p < a->g.npiles; p++) {
        v->slots[p] = lv_image_create(slots);
        lv_obj_remove_flag(v->slots[p], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(v->slots[p], LV_OBJ_FLAG_HIDDEN);
    }

    v->layer = plain(a->root);
    lv_obj_set_size(v->layer, v->W, v->H);
    for (int i = 0; i < 2; i++) {
        v->shadow[i] = plain(v->layer);
        lv_obj_set_style_bg_color(v->shadow[i], lv_color_hex(0), 0);
        lv_obj_set_style_bg_opa(v->shadow[i], i ? 34 : 46, 0);
        lv_obj_set_style_radius(v->shadow[i], v->cw / 12 + i * 4, 0);
        lv_obj_add_flag(v->shadow[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < a->g.ncards; i++) {
        lv_obj_t *c = lv_image_create(v->layer);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_antialias(c, true);
        v->card[i] = c;
    }
    for (int i = a->g.ncards; i < SO_MAXCARDS; i++) {
        v->card[i] = NULL;
    }

    lv_obj_t *fx = plain(a->root);
    lv_obj_set_size(fx, v->W, v->H);
    for (int i = 0; i < SO_MAX_HL; i++) {
        v->hl[i] = outline(fx, 0xFFD54A, 4);
        lv_obj_set_style_bg_color(v->hl[i], lv_color_hex(0xFFF3B0), 0);
        lv_obj_set_style_bg_opa(v->hl[i], 0, 0);
    }
    v->hint_a = outline(fx, 0x7CF0FF, 5);
    v->hint_b = outline(fx, 0x7CF0FF, 5);
    v->cursor = outline(fx, 0xFFFFFF, 5);
    v->ring = outline(fx, 0xFFE08A, 5);
    return true;
}

void so_view_free(so_app_t *a)
{
    so_view_t *v = &a->v;
    images_free(v);
    so_art_free(&v->art);
    free(v->feltmem);
    free(v->feltclean);
    v->feltmem = v->feltclean = NULL;
    for (int i = 0; i < SO_MAXCARDS; i++) {
        v->card[i] = NULL;
    }
}

/* --- the looks changed ----------------------------------------------------------------- */

void so_view_set_looks(so_app_t *a)
{
    so_view_t *v = &a->v;
    if (v->felt_id != a->set.felt && v->feltmem) {
        so_felt_draw565(v->feltclean, v->W, v->H, a->set.felt);
        memcpy(v->feltmem, v->feltclean, (size_t)v->W * v->H * 2);
        v->felt_id = a->set.felt;
        lv_obj_invalidate(v->felt);
    }
    if (v->art_deck != a->set.deck && !v->deck_kind) {
        v->art_cw = 0;
        images_make(a);
        for (int i = 0; i < a->g.ncards; i++) {
            if (v->card[i]) {
                lv_image_set_src(v->card[i], v->cv[i].up ? so_view_face(a, i) : &v->back_d);
            }
        }
    }
    if (v->back_id != a->set.back && v->back.px) {
        lv_image_cache_drop(&v->back_d);
        so_back_draw(&v->art, &v->back, a->set.back);
        v->back_id = a->set.back;
        for (int i = 0; i < a->g.ncards; i++) {
            if (v->card[i]) lv_obj_invalidate(v->card[i]);
        }
    }
}

/* --- placing ---------------------------------------------------------------------------- */

static void card_put(so_view_t *v, int i)
{
    so_cv_t *c = &v->cv[i];
    lv_obj_set_pos(v->card[i], (int32_t)floorf(c->x + 0.5f) - v->art.pad, (int32_t)floorf(c->y + 0.5f) - v->art.pad);
}

static void card_face(so_app_t *a, int i, bool up)
{
    so_cv_t *c = &a->v.cv[i];
    c->up = up;
    lv_image_set_src(a->v.card[i], up ? (const void *)so_view_face(a, i) : (const void *)&a->v.back_d);
}

/* the canonical order of drawing: piles in a fixed order, then by index */
static int pile_rank(const so_game_t *g, int p)
{
    switch (g->ptype[p]) {
    case SO_P_STOCK:   return 0;
    case SO_P_DISCARD: return 1;
    case SO_P_WASTE:   return 2;
    case SO_P_FOUND:   return 3;
    case SO_P_CELL:    return 4;
    case SO_P_TABLEAU: return 5;
    default:           return 6;
    }
}

static void restack(so_app_t *a)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    int k = 2;                          /* after the two lift shadows */
    for (int r = 0; r <= 6; r++) {
        for (int p = 0; p < g->npiles; p++) {
            if (pile_rank(g, p) != r) continue;
            for (int i = 0; i < g->pn[p]; i++) {
                lv_obj_t *o = v->card[g->pc[p][i]];
                if (lv_obj_get_index(o) != k) {
                    lv_obj_move_to_index(o, k);
                }
                k++;
            }
        }
    }
}

static void slots_refresh(so_app_t *a)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    for (int p = 0; p < g->npiles; p++) {
        lv_obj_t *o = v->slots[p];
        if (!o) continue;
        int kind = v->pl[p].slot;
        if (g->ptype[p] == SO_P_STOCK && kind != 255) {
            kind = so_stock_move(g, NULL) || g->pn[p] ? SO_SLOT_STOCK : SO_SLOT_DONE;
            if (g->kind == SO_K_TRIPEAKS || g->kind == SO_K_GOLF) kind = SO_SLOT_DONE;
        }
        if (kind == 255 || v->pl[p].off) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_image_set_src(o, &v->slot_d[kind]);
        lv_obj_set_pos(o, v->pl[p].x - v->art.pad, v->pl[p].y - v->art.pad);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Everything straight where it belongs: a new hand (face down at the stock
 * position first if 'deal'), or a view built again. */
void so_view_reset(so_app_t *a, bool deal)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    slots_refresh(a);
    float sx = (float)(v->W / 2 - v->cw / 2), sy = (float)(v->H + 20);
    if (g->stock >= 0) {
        sx = v->pl[g->stock].x;
        sy = v->pl[g->stock].y;
    }
    int order = 0;
    /* deal in the order the cards were laid: round by round over the columns */
    for (int p = 0; p < g->npiles; p++) {
        for (int i = 0; i < g->pn[p]; i++) {
            int c = g->pc[p][i];
            so_cv_t *cv = &v->cv[c];
            float x, y;
            so_view_card_pos(a, p, i, &x, &y);
            cv->pile = (int8_t)p;
            cv->idx = (uint8_t)i;
            cv->moving = cv->flipping = false;
            cv->pop = 0;
            cv->rot = 0;
            lv_image_set_rotation(v->card[c], 0);
            lv_image_set_scale(v->card[c], LV_SCALE_NONE);
            lv_obj_remove_flag(v->card[c], LV_OBJ_FLAG_HIDDEN);
            if (deal && g->ptype[p] != SO_P_STOCK) {
                cv->x = sx;
                cv->y = sy;
                card_face(a, c, false);
                cv->x0 = sx;
                cv->y0 = sy;
                cv->x1 = x;
                cv->y1 = y;
                cv->t = 0;
                cv->dur = 260;
                cv->ease = SO_EASE_DEAL;
                cv->moving = true;
                /* a column at a time is how a hand deals: by index first */
                int rnk = g->ptype[p] == SO_P_TABLEAU ? i * 16 + (p - g->tab0) : 200 + p * 2 + i;
                if (g->ptype[p] == SO_P_SLOT) rnk = p - g->slot0;
                cv->delay = (int16_t)(rnk * 26);
                (void)order;
                if (g->up[c]) {
                    cv->flipping = true;
                    cv->flip_to = true;
                    cv->ft = 0;
                    cv->fdelay = (int16_t)(cv->delay + 200);
                }
            } else {
                cv->x = cv->x1 = x;
                cv->y = cv->y1 = y;
                card_face(a, c, g->up[c]);
            }
            card_put(v, c);
        }
    }
    restack(a);
    v->busy = deal;
    v->settling = deal;
    a->last_ops = g->nops;
}

/* After a move: every card whose place or face changed goes there, in the
 * order the log names them, 'stagger' ms apart. */
void so_view_sync(so_app_t *a, uint32_t op_from, int stagger)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    /* The order the cards moved in: replay the piles backwards from now to
     * 'op_from', noting for each card the last operation that carried it.
     * Cards carried together (a run) share a number, and fly together. */
    int16_t seq[SO_MAXCARDS];
    static uint8_t pc[SO_MAXPILES][SO_PILE_MAX];
    static uint8_t pn[SO_MAXPILES];
    int16_t opk[SO_MAXCARDS];
    for (int i = 0; i < g->ncards; i++) {
        seq[i] = -1;
        opk[i] = -1;
    }
    memcpy(pc, g->pc, sizeof pc);
    memcpy(pn, g->pn, sizeof pn);
    for (uint32_t k = g->nops; k > op_from; k--) {
        const so_op_t *o = &g->ops[k - 1];
        if (o->op != SO_OP_MOVE) continue;
        for (int j = 0; j < o->n; j++) {
            int c = pc[o->b][pn[o->b] - o->n + j];
            if (opk[c] < 0) opk[c] = (int16_t)(k - 1 - op_from);
        }
        memcpy(&pc[o->a][pn[o->a]], &pc[o->b][pn[o->b] - o->n], (size_t)o->n);
        pn[o->a] = (uint8_t)(pn[o->a] + o->n);
        pn[o->b] = (uint8_t)(pn[o->b] - o->n);
    }
    /* the operations' numbers, packed: 0, 1, 2... in order */
    for (int c = 0; c < g->ncards; c++) {
        if (opk[c] < 0) continue;
        int r = 0;
        for (int k = 0; k < opk[c]; k++) {
            for (int d = 0; d < g->ncards; d++) {
                if (opk[d] == k) {
                    r++;
                    break;
                }
            }
        }
        seq[c] = (int16_t)r;
    }

    for (int p = 0; p < g->npiles; p++) {
        for (int i = 0; i < g->pn[p]; i++) {
            int c = g->pc[p][i];
            so_cv_t *cv = &v->cv[c];
            float x, y;
            so_view_card_pos(a, p, i, &x, &y);
            bool moved = cv->pile != p || cv->idx != i;
            bool land_home = moved && (g->ptype[p] == SO_P_FOUND || g->ptype[p] == SO_P_DISCARD);
            cv->pile = (int8_t)p;
            cv->idx = (uint8_t)i;
            int delay = seq[c] >= 0 ? seq[c] * stagger : 0;
            float tx = cv->moving ? cv->x1 : cv->x, ty = cv->moving ? cv->y1 : cv->y;
            if (fabsf(x - tx) > 0.5f || fabsf(y - ty) > 0.5f) {
                cv->x0 = cv->x;
                cv->y0 = cv->y;
                cv->x1 = x;
                cv->y1 = y;
                cv->t = 0;
                float dx = x - cv->x, dy = y - cv->y;
                float d = sqrtf(dx * dx + dy * dy);
                int dur = (int)(150.f + d * 0.22f);
                cv->dur = (int16_t)(dur > 380 ? 380 : dur);
                cv->delay = (int16_t)delay;
                cv->ease = SO_EASE_OUT;
                if (!cv->moving || seq[c] >= 0) {
                    cv->moving = true;
                    if (delay <= 0) {
                        lv_obj_move_to_index(v->card[c], -1);
                    }
                }
                v->busy = true;
                v->settling = true;
            } else if (seq[c] >= 0 && !cv->moving) {
                cv->x1 = x;
                cv->y1 = y;
            }
            if (land_home) {
                cv->pop = (int16_t)(-(cv->delay + cv->dur));   /* starts when it lands */
            }
            if ((bool)g->up[c] != (cv->flipping ? cv->flip_to : cv->up)) {
                cv->flipping = true;
                cv->flip_to = g->up[c];
                cv->ft = 0;
                cv->fdelay = (int16_t)(cv->moving ? cv->delay + cv->dur / 2 : delay);
                v->busy = true;
            }
        }
    }
    slots_refresh(a);
    a->last_ops = g->nops;
}

/* --- motion ----------------------------------------------------------------------------- */

static float ease(int kind, float p)
{
    switch (kind) {
    case SO_EASE_SPRING:
        return 1.f - expf(-6.5f * p) * cosf(p * 11.f);
    case SO_EASE_INOUT:
        return p < 0.5f ? 2.f * p * p : 1.f - 2.f * (1.f - p) * (1.f - p);
    default:
        return 1.f - (1.f - p) * (1.f - p) * (1.f - p);
    }
}

static void ring_flash(so_app_t *a, int pile, bool big)
{
    so_view_t *v = &a->v;
    v->ring_t = big ? RING_MS : RING_MS * 2 / 3;
    lv_obj_set_user_data(v->ring, (void *)(intptr_t)pile);
    lv_obj_set_style_border_color(v->ring, lv_color_hex(big ? 0xFFD54A : 0xFFFFFF), 0);
    lv_obj_remove_flag(v->ring, LV_OBJ_FLAG_HIDDEN);
}

void so_view_frame(so_app_t *a, int dt)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    bool any = false;
    for (int i = 0; i < g->ncards; i++) {
        so_cv_t *c = &v->cv[i];
        lv_obj_t *o = v->card[i];
        if (!o) continue;
        if (c->moving) {
            any = true;
            if (c->delay > 0) {
                c->delay = (int16_t)(c->delay - dt);
                if (c->delay <= 0) {
                    lv_obj_move_to_index(o, -1);
                }
            } else {
                c->t = (int16_t)(c->t + dt);
                float p = c->dur ? (float)c->t / (float)c->dur : 1.f;
                if (p >= 1.f) {
                    c->x = c->x1;
                    c->y = c->y1;
                    c->moving = false;
                } else {
                    float e = ease(c->ease, p);
                    c->x = c->x0 + (c->x1 - c->x0) * e;
                    c->y = c->y0 + (c->y1 - c->y0) * e;
                    if (c->ease == SO_EASE_OUT || c->ease == SO_EASE_DEAL) {
                        /* a little hop on longer flights */
                        float dx = c->x1 - c->x0, dy = c->y1 - c->y0;
                        float d = sqrtf(dx * dx + dy * dy);
                        if (d > 120.f) {
                            c->y -= sinf(p * 3.14159265f) * (d * 0.08f > 40.f ? 40.f : d * 0.08f);
                        }
                    }
                }
                card_put(v, i);
            }
        }
        if (c->flipping) {
            any = true;
            if (c->fdelay > 0) {
                c->fdelay = (int16_t)(c->fdelay - dt);
            } else {
                c->ft = (int16_t)(c->ft + dt);
                int sx, sy;
                if (c->ft < FLIP_MS) {
                    float q = (float)c->ft / FLIP_MS;
                    sx = (int)(256.f * cosf(q * 1.5707963f));
                    sy = 256 + (int)(20.f * sinf(q * 1.5707963f));
                } else {
                    if (c->up != c->flip_to) {
                        card_face(a, i, c->flip_to);
                    }
                    float q = (float)(c->ft - FLIP_MS) / FLIP_MS;
                    if (q > 1.f) q = 1.f;
                    sx = (int)(256.f * sinf(q * 1.5707963f));
                    sy = 256 + (int)(20.f * cosf(q * 1.5707963f));
                }
                if (c->ft >= FLIP_MS * 2) {
                    c->flipping = false;
                    sx = sy = LV_SCALE_NONE;
                }
                lv_image_set_scale_x(o, (uint32_t)(sx < 10 ? 10 : sx));
                lv_image_set_scale_y(o, (uint32_t)sy);
            }
        }
        if (c->pop) {
            if (c->pop < 0) {
                c->pop = (int16_t)(c->pop + dt);
                if (c->pop >= 0) {
                    c->pop = POP_MS;
                    int p = c->pile;
                    bool full = g->ptype[p] == SO_P_FOUND && g->pn[p] == (g->kind == SO_K_SPANISH ? 10 : 13) &&
                                c->idx == g->pn[p] - 1;
                    if (g->ptype[p] == SO_P_FOUND) ring_flash(a, p, full);
                    so_beep(a, full ? 2 : 1);
                }
            } else {
                any = true;
                c->pop = (int16_t)(c->pop - dt);
                if (c->pop <= 0) {
                    c->pop = 0;
                    if (!c->flipping) lv_image_set_scale(o, LV_SCALE_NONE);
                } else if (!c->flipping) {
                    float q = 1.f - (float)c->pop / POP_MS;
                    lv_image_set_scale(o, (uint32_t)(256.f + 34.f * sinf(q * 3.14159265f)));
                }
            }
        }
    }
    if (v->ring_t > 0) {
        v->ring_t = (int16_t)(v->ring_t - dt);
        int p = (int)(intptr_t)lv_obj_get_user_data(v->ring);
        float q = 1.f - (float)v->ring_t / RING_MS;
        int grow = (int)(q * (float)v->cw * 0.35f);
        lv_obj_set_pos(v->ring, v->pl[p].x - grow, v->pl[p].y - grow);
        lv_obj_set_size(v->ring, v->cw + 2 * grow, v->ch + 2 * grow);
        lv_obj_set_style_radius(v->ring, v->cw / 10 + grow / 2, 0);
        lv_obj_set_style_border_opa(v->ring, (lv_opa_t)(255.f * (1.f - q)), 0);
        if (v->ring_t <= 0) lv_obj_add_flag(v->ring, LV_OBJ_FLAG_HIDDEN);
    }
    v->busy = any;
    if (!any && v->settling && !a->dragging) {
        v->settling = false;
        restack(a);
    }
}

/* --- touch ------------------------------------------------------------------------------ */

int so_view_hit(so_app_t *a, int x, int y, int *idx)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    /* from the top of the drawing order down */
    for (int r = 6; r >= 0; r--) {
        for (int p = g->npiles - 1; p >= 0; p--) {
            if (pile_rank(g, p) != r || v->pl[p].off) continue;
            for (int i = g->pn[p] - 1; i >= 0; i--) {
                so_cv_t *c = &v->cv[g->pc[p][i]];
                if (x >= c->x && x < c->x + v->cw && y >= c->y && y < c->y + v->ch) {
                    *idx = i;
                    return p;
                }
            }
            if (!g->pn[p] && v->pl[p].slot != 255 && x >= v->pl[p].x && x < v->pl[p].x + v->cw &&
                y >= v->pl[p].y && y < v->pl[p].y + v->ch) {
                *idx = -1;
                return p;
            }
        }
    }
    return -1;
}

/* the area a pile takes for a drop: its last card, a column down to its end */
static void drop_rect(so_app_t *a, int p, lv_area_t *r)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    float x = v->pl[p].x, y = v->pl[p].y;
    if (g->pn[p]) {
        so_cv_t *c = &v->cv[g->pc[p][g->pn[p] - 1]];
        x = c->x1;
        y = c->y1;
        if (!c->moving) {
            x = c->x;
            y = c->y;
        }
    }
    r->x1 = (int32_t)x - 6;
    r->y1 = (int32_t)y - 6;
    r->x2 = (int32_t)x + v->cw + 6;
    r->y2 = (int32_t)y + v->ch + 6;
    if (g->ptype[p] == SO_P_TABLEAU && g->kind != SO_K_GOLF) {
        r->y1 = v->pl[p].y - 6;
        if (r->y2 < v->pl[p].y + v->pl[p].room) r->y2 = v->pl[p].y + v->pl[p].room;
    }
}

int so_view_drop_pile(so_app_t *a, int x, int y)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    /* the card being carried: its rectangle against each legal target */
    int best = -1, bestov = 0;
    so_cv_t *top = a->pick_pile >= 0 && a->pick_idx >= 0 ? &v->cv[g->pc[a->pick_pile][a->pick_idx]] : NULL;
    lv_area_t cr = { x - v->cw / 2, y - v->ch / 3, x + v->cw / 2, y + v->ch * 2 / 3 };
    if (top) {
        cr = (lv_area_t){ (int32_t)top->x, (int32_t)top->y, (int32_t)top->x + v->cw,
                          (int32_t)top->y + v->ch };
    }
    for (int p = 0; p < g->npiles; p++) {
        if (!a->legal_to[p]) continue;
        lv_area_t r, o;
        drop_rect(a, p, &r);
        o.x1 = r.x1 > cr.x1 ? r.x1 : cr.x1;
        o.y1 = r.y1 > cr.y1 ? r.y1 : cr.y1;
        o.x2 = r.x2 < cr.x2 ? r.x2 : cr.x2;
        o.y2 = r.y2 < cr.y2 ? r.y2 : cr.y2;
        if (o.x2 <= o.x1 || o.y2 <= o.y1) continue;
        int ov = (o.x2 - o.x1) * (o.y2 - o.y1);
        bool finger = x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
        if (finger) ov += 100000;
        if (ov > bestov) {
            bestov = ov;
            best = p;
        }
    }
    return best;
}

/* the carried cards follow the finger, leaning with its speed */
void so_view_drag(so_app_t *a, int p, int idx, int x, int y, float tilt)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    float bx = (float)(x - a->grab_dx), by = (float)(y - a->grab_dy);
    float fy = (float)v->pl[p].dy;
    if (g->ptype[p] != SO_P_TABLEAU) fy = (float)v->art.strip;
    int n = g->pn[p] - idx;
    for (int k = 0; k < n; k++) {
        int c = g->pc[p][idx + k];
        so_cv_t *cv = &v->cv[c];
        cv->moving = false;
        cv->x = bx;
        cv->y = by + (float)k * fy;
        card_put(v, c);
        int r = (int)(tilt * 10.f);
        if (r != cv->rot) {
            cv->rot = (int16_t)r;
            lv_image_set_pivot(v->card[c], a->grab_dx + v->art.pad,
                               a->grab_dy - (int)((float)k * fy) + v->art.pad);
            lv_image_set_rotation(v->card[c], r);
        }
    }
    float h = (float)v->ch + (float)(n - 1) * fy;
    for (int i = 0; i < 2; i++) {
        int g2 = i * 4;
        lv_obj_set_pos(v->shadow[i], (int32_t)bx + 7 - g2, (int32_t)by + 12 - g2);
        lv_obj_set_size(v->shadow[i], v->cw + 2 * g2, (int32_t)h + 2 * g2);
    }
}

void so_view_lift(so_app_t *a, int p, int idx, bool on)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    if (on) {
        for (int i = 0; i < 2; i++) {
            lv_obj_remove_flag(v->shadow[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_to_index(v->shadow[i], -1);
        }
        for (int k = idx; k < g->pn[p]; k++) {
            lv_obj_move_to_index(v->card[g->pc[p][k]], -1);
        }
    } else {
        for (int i = 0; i < 2; i++) {
            lv_obj_add_flag(v->shadow[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_to_index(v->shadow[i], i);
        }
        for (int i = 0; i < g->ncards; i++) {
            if (v->cv[i].rot) {
                v->cv[i].rot = 0;
                lv_image_set_rotation(v->card[i], 0);
            }
        }
        v->settling = true;
    }
}

static void frame_obj(so_app_t *a, lv_obj_t *o, int p, int idx, int grow)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    float x = v->pl[p].x, y = v->pl[p].y, y2;
    int n = g->pn[p];
    if (n) {
        if (idx < 0 || idx >= n) idx = n - 1;
        so_cv_t *c0 = &v->cv[g->pc[p][idx]], *c1 = &v->cv[g->pc[p][n - 1]];
        x = c0->moving ? c0->x1 : c0->x;
        y = c0->moving ? c0->y1 : c0->y;
        y2 = (c1->moving ? c1->y1 : c1->y) + (float)v->ch;
        if (g->ptype[p] != SO_P_TABLEAU) y2 = y + (float)v->ch;
    } else {
        y2 = y + (float)v->ch;
    }
    lv_obj_set_pos(o, (int32_t)x - grow, (int32_t)y - grow);
    lv_obj_set_size(o, v->cw + 2 * grow, (int32_t)(y2 - y) + 2 * grow);
    lv_obj_set_style_radius(o, v->cw / 12 + grow, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void so_view_highlight(so_app_t *a, const bool *legal, int hover)
{
    so_view_t *v = &a->v;
    int k = 0;
    for (int p = 0; legal && p < a->g.npiles && k < SO_MAX_HL; p++) {
        if (!legal[p]) continue;
        lv_obj_t *o = v->hl[k++];
        frame_obj(a, o, p, -1, 4);
        if (a->g.ptype[p] == SO_P_TABLEAU && a->g.pn[p]) {
            /* only the card it would land on */
            int n = a->g.pn[p];
            so_cv_t *c = &v->cv[a->g.pc[p][n - 1]];
            lv_obj_set_pos(o, (int32_t)c->x - 4, (int32_t)c->y - 4);
            lv_obj_set_size(o, v->cw + 8, v->ch + 8);
        }
        bool h = p == hover;
        lv_obj_set_style_border_width(o, h ? 7 : 4, 0);
        lv_obj_set_style_border_opa(o, h ? LV_OPA_COVER : 170, 0);
        lv_obj_set_style_bg_opa(o, h ? 50 : 0, 0);
    }
    for (; k < SO_MAX_HL; k++) {
        lv_obj_add_flag(v->hl[k], LV_OBJ_FLAG_HIDDEN);
    }
}

void so_view_hint(so_app_t *a, const so_move_t *m, bool on)
{
    so_view_t *v = &a->v;
    so_game_t *g = &a->g;
    if (!on || !m) {
        lv_obj_add_flag(v->hint_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->hint_b, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (m->kind == SO_MV_DRAW || m->kind == SO_MV_REDEAL) {
        frame_obj(a, v->hint_a, g->stock, -1, 6);
        lv_obj_add_flag(v->hint_b, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (m->kind == SO_MV_PAIR) {
        frame_obj(a, v->hint_a, m->from, -1, 6);
        if (m->to != 255) frame_obj(a, v->hint_b, m->to, -1, 6);
        else lv_obj_add_flag(v->hint_b, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    frame_obj(a, v->hint_a, m->from, g->pn[m->from] - m->n, 6);
    frame_obj(a, v->hint_b, m->to, -1, 6);
}

void so_view_cursor(so_app_t *a, int p, int idx, bool held)
{
    so_view_t *v = &a->v;
    if (p < 0) {
        lv_obj_add_flag(v->cursor, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    frame_obj(a, v->cursor, p, idx, 6);
    lv_obj_set_style_border_color(v->cursor, lv_color_hex(held ? 0xFFD54A : 0xFFFFFF), 0);
    lv_obj_move_to_index(v->cursor, -1);
}

void so_view_hide_cards(so_app_t *a, bool hide)
{
    so_view_t *v = &a->v;
    for (int i = 0; i < a->g.ncards; i++) {
        if (!v->card[i]) continue;
        if (hide) lv_obj_add_flag(v->card[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(v->card[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int p = 0; p < a->g.npiles; p++) {
        if (v->slots[p] && hide) lv_obj_add_flag(v->slots[p], LV_OBJ_FLAG_HIDDEN);
    }
    if (!hide) slots_refresh(a);
}
