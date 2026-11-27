/*
 * Marcador - the flip score card.
 *
 * The stand that sits at the side of a table-tennis table or a padel court:
 * an A-frame with a bar of rings on top, and on the rings stacks of cards
 * with big numbers, one stack per player for the points and small ones for
 * games and sets. To give a point the top card is lifted over the rings and
 * falls down the back, and the next number shows.
 *
 * The flip is drawn as the card turning about its rings: while it comes
 * towards you its face shortens upwards and darkens (cos of the angle, and
 * a little wider as it nears), past the top you see its back standing over
 * the rings, and then it falls away behind. Going back (undo, minus) is the
 * same turn the other way. The paper flaps and slaps the stand.
 *
 * The rules are the model's (mr_point): table tennis to eleven by two and
 * games, padel and tennis with fifteen, thirty, forty, deuce, advantage,
 * games, sets and the tie-break, or plain numbers. Tap a player's big card
 * for a point; the minus takes one back.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLIP_S  0.46f
#define NSTK    6           /* two big, four small                         */

typedef struct {
    int       x, y, w, h;   /* the card; y is the rings                     */
    uint32_t  ink;
    char      cur[6], shown[6], pend[6];
    mr_img_t  im_stat, im_flip;
    lv_obj_t *stat, *flip, *back;
    bool      on, rev, used;
    float     t;
    int       big;
} stk_t;

typedef struct {
    mr_cv_t   bg;
    mr_img_t  back[2];      /* the back of a big and of a small card        */
    stk_t     k[NSTK];
    int       mode;
    struct {
        int who;
    } f[MR_FINGERS];
} car_t;

#define ST ((car_t *)s->st)

static void car_btn(mr_scene_t *s, int arg);

/* --- drawing ------------------------------------------------------------------------- */

static void card_draw(mr_img_t *im, const char *txt, uint32_t ink, bool back)
{
    int w = im->w, h = im->h;
    memset(im->px, 0, (size_t)w * h * 4);
    float rad = w * 0.06f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = mr_rrect_sd(x + 0.5f, y + 0.5f, 0, 0, (float)w, (float)h, rad);
            float c = mr_clampf(0.5f - d, 0, 1);
            if (c <= 0) continue;
            /* thick card: a soft sheen and paper tooth */
            float n = (mr_hashf(x, y, 61) - 0.5f) * 0.035f;
            float sh = 1.0f - 0.07f * (float)y / h + n;
            uint32_t col = back ? mr_rgb_scale(0xDAD6CC, sh) : mr_rgb_scale(0xFBFAF4, sh);
            if (d > -3) col = mr_rgb_scale(col, 0.88f);
            im->px[y * w + x] = ((uint32_t)(c * 255) << 24) | col;
        }
    }
    /* the punched holes for the rings */
    for (int k = 0; k < 2; k++) {
        float hx = w * (k ? 0.75f : 0.25f), hy = h * 0.055f + 6;
        for (int y = (int)(hy - 10); y < (int)(hy + 10); y++)
            for (int x = (int)(hx - 10); x < (int)(hx + 10); x++) {
                float d = sqrtf((x + 0.5f - hx) * (x + 0.5f - hx) + (y + 0.5f - hy) * (y + 0.5f - hy));
                float r = w * 0.035f + 2;
                if (d < r + 1 && x >= 0 && y >= 0 && x < w && y < h) {
                    uint32_t a = (uint32_t)(255 * mr_clampf(d - r + 0.5f, 0, 1));
                    uint32_t p = im->px[y * w + x];
                    im->px[y * w + x] = ((((p >> 24) * a) / 255) << 24) | (p & 0xFFFFFF);
                }
            }
    }
    if (back) {
        /* a faint printed line on the back */
        for (int y = (int)(h * 0.45f); y < (int)(h * 0.55f); y += 6)
            for (int x = (int)(w * 0.2f); x < (int)(w * 0.8f); x++) mr_img_over(im, x, y, 0x9A968C, 0.25f);
        return;
    }
    size_t n = strlen(txt);
    if (!n) return;
    float gh = h * (n > 2 ? 0.50f : 0.62f), lw = gh * 0.15f;
    float tw = mr_text_w(txt, gh);
    if (tw > w * 0.86f) {
        gh *= w * 0.86f / tw;
        lw = gh * 0.15f;
        tw = mr_text_w(txt, gh);
    }
    mr_text_img(im, txt, (w - tw) * 0.5f, h * 0.56f - gh * 0.5f, gh, lw, ink);
}

static void stand_draw(mr_scene_t *s)
{
    car_t *C = ST;
    mr_cv_t *cv = &C->bg;
    int w = cv->w, h = cv->h;
    /* the table: blue, the white edge line, the net's shadow across the far end */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float n = (mr_hashf(x, y, 7) - 0.5f) * 0.05f + (mr_vnoise(x / 70.0f, y / 70.0f, 8) - 0.5f) * 0.06f;
            float light = 1.08f - 0.30f * fabsf((float)x / w - 0.5f) - 0.18f * (float)y / h;
            uint32_t c = mr_rgb_scale(0x1B4F8A, light + n);
            /* the table's lacquer catches the hall's lights */
            float sp = expf(-((x - w * 0.3f) * (x - w * 0.3f) + (y - h * 0.25f) * (y - h * 0.25f)) / (w * w * 0.08f));
            c = mr_rgb_mix(c, 0xBFD8F4, sp * 0.18f);
            cv->px[y * w + x] = mr_565(c);
        }
    int lw = 10, in = 22;
    for (int y = s->top; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool line = (x >= in && x < in + lw) || (x >= w - in - lw && x < w - in) || (y >= h - in - lw && y < h - in) ||
                        (abs(x - w / 2) < 2);
            if (line) mr_cv_mix(cv, x, y, 0xF2F4F6, 0.85f);
        }
    /* the stand: a dark plastic front behind each player's stacks, on two feet */
    for (int p = 0; p < 2; p++) {
        int x0 = C->k[p].x - 34, x1 = C->k[p].x + C->k[p].w + 34, y0 = C->k[p].y - 34, y1 = C->k[p].y + C->k[p].h + 40;
        for (int i = 2; i < NSTK; i++)
            if (C->k[i].used && (i & 1) == p) {
                if (C->k[i].y + C->k[i].h + 54 > y1) y1 = C->k[i].y + C->k[i].h + 54;
                if (C->k[i].x + C->k[i].w + 30 > x1) x1 = C->k[i].x + C->k[i].w + 30;
            }
        for (int y = y0 + 10; y < y1 + 16 && y < h; y++)
            for (int x = x0 + 12; x < x1 + 12 && x < w; x++) mr_cv_mix(cv, x, y, 0x000000, 0.25f);
        for (int y = y0; y < y1 && y < h; y++)
            for (int x = x0; x < x1; x++) {
                float d = mr_rrect_sd(x + 0.5f, y + 0.5f, (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), 18);
                float c = mr_clampf(0.5f - d, 0, 1);
                if (c <= 0) continue;
                float sh = 0.9f + 0.25f * (1.0f - (float)(y - y0) / (y1 - y0)) + (mr_hashf(x, y, 5) - 0.5f) * 0.04f;
                if (d > -4) sh *= 1.35f;
                mr_cv_mix(cv, x, y, mr_rgb_scale(0x24272D, sh), c);
            }
        for (int k = 0; k < 2; k++) {
            int fx = k ? x1 - 60 : x0 + 20;
            for (int y = y1; y < y1 + 14 && y < h; y++)
                for (int x = fx; x < fx + 40; x++) mr_cv_mix(cv, x, y, 0x15171A, 1.0f);
        }
    }
    /* the stacks' shadow and edges */
    for (int i = 0; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->used) continue;
        /* the stack's thickness: edges of the cards under the top one */
        for (int e = 1; e <= 5; e++)
            for (int x = k->x + 2; x < k->x + k->w - 2; x++) {
                int y = k->y + k->h + e * 2;
                mr_cv_mix(cv, x, y, e & 1 ? 0xE8E6DE : 0xB0ADA4, 0.95f);
                mr_cv_mix(cv, x, y + 1, 0x000000, 0.25f);
            }
        for (int y = k->y + 8; y < k->y + k->h + 26; y++)
            for (int x = k->x + 8; x < k->x + k->w + 16; x++) mr_cv_mix(cv, x, y, 0x000000, 0.22f);
    }
    (void)lw;
}

/* the bar with the rings, over every stack */
static void rings_draw(mr_scene_t *s, lv_obj_t *parent)
{
    car_t *C = ST;
    for (int i = 0; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->used) continue;
        lv_obj_t *bar = lv_obj_create(parent);
        mr_obj_plain(bar);
        int bh = k->big ? 30 : 22;
        lv_obj_set_size(bar, k->w + 24, bh);
        lv_obj_set_pos(bar, k->x - 12, k->y - bh / 2 - 6);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2D33), 0);
        lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x101114), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(bar, 255, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_set_style_shadow_width(bar, 12, 0);
        lv_obj_set_style_shadow_offset_y(bar, 5, 0);
        lv_obj_set_style_shadow_opa(bar, 140, 0);
        for (int r = 0; r < 2; r++) {
            lv_obj_t *ring = lv_obj_create(parent);
            mr_obj_plain(ring);
            int rw = k->big ? 26 : 20, rh = k->big ? 46 : 34;
            lv_obj_set_size(ring, rw, rh);
            lv_obj_set_pos(ring, k->x + (int)(k->w * (r ? 0.75f : 0.25f)) - rw / 2, k->y - rh / 2);
            lv_obj_set_style_radius(ring, rw / 2, 0);
            lv_obj_set_style_border_width(ring, 5, 0);
            lv_obj_set_style_border_color(ring, lv_color_hex(0xC9CED6), 0);
            lv_obj_set_style_border_opa(ring, 255, 0);
            lv_obj_set_style_bg_opa(ring, 0, 0);
            lv_obj_set_style_shadow_width(ring, 4, 0);
            lv_obj_set_style_shadow_opa(ring, 120, 0);
        }
    }
    (void)s;
}

/* --- what each stack shows ---------------------------------------------------------------- */

static void texts(mr_scene_t *s, char out[NSTK][6])
{
    car_t *C = ST;
    mr_game_t *g = s->g;
    memset(out, 0, NSTK * 6);
    for (int p = 0; p < 2; p++) {
        if (C->mode == MR_CAR_PADEL) snprintf(out[p], 6, "%s", mr_tennis_pts(g, p));
        else snprintf(out[p], 6, "%d", (int)(g->p[p].score % 1000));
        if (C->mode == MR_CAR_PADEL) {
            snprintf(out[2 + p], 6, "%d", g->p[p].aux[0] % 100);
            snprintf(out[4 + p], 6, "%d", g->p[p].aux[1] % 100);
        } else if (C->mode == MR_CAR_PING) {
            snprintf(out[2 + p], 6, "%d", g->p[p].aux[1] % 100);
        }
    }
}

static void stack_show(stk_t *k, const char *txt)
{
    snprintf(k->shown, sizeof k->shown, "%s", txt);
    card_draw(&k->im_stat, txt, k->ink, false);
    lv_image_cache_drop(&k->im_stat.dsc);
    lv_obj_invalidate(k->stat);
}

/* --- build ------------------------------------------------------------------------------------ */

static void build(mr_scene_t *s)
{
    car_t *C = (car_t *)mr_alloc(sizeof(car_t));
    s->st = C;
    if (!C) return;
    mr_game_t *g = s->g;
    C->mode = g->opt[MR_CAR_MODE];
    int top = s->top;
    float W = s->w, H = s->h - top;
    /* big cards: two side by side, the small ones under them */
    int nsmall = C->mode == MR_CAR_PADEL ? 2 : C->mode == MR_CAR_PING ? 1 : 0;
    float bw, bh, sw, sh, y0;
    bool side = s->land;        /* lying down the small stacks go beside the big one */
    if (side) {
        bh = mr_minf(H - 70 - 40 - 120, 460);
        bw = bh / 1.32f;
        sw = bw * 0.42f;
        sh = sw * 1.32f;
        float room = W * 0.5f - 90;
        if (bw + (nsmall ? 40 + sw : 0) > room) {
            float k = room / (bw + (nsmall ? 40 + sw : 0));
            bw *= k, bh *= k, sw *= k, sh *= k;
        }
        y0 = top + (H - (bh + 40 + 120)) * 0.5f + 60;
    } else {
        bw = mr_minf(W * 0.5f - 110, 300);
        bh = bw * 1.32f;
        sw = bw * 0.46f;
        sh = sw * 1.32f;
        float need = 70 + bh + 40 + (nsmall ? 70 + sh : 0) + 120;
        if (need > H) {
            float k = (H - 70 - 40 - 120 - (nsmall ? 70 : 0)) / (bh + (nsmall ? sh : 0));
            bw *= k, bh *= k, sw *= k, sh *= k;
            need = 70 + bh + 40 + (nsmall ? 70 + sh : 0) + 120;
        }
        y0 = top + (H - need) * 0.5f + 70;
    }
    for (int p = 0; p < 2; p++) {
        float cx = W * (p ? 0.75f : 0.25f);
        stk_t *k = &C->k[p];
        k->used = true;
        k->big = 1;
        k->w = (int)bw;
        k->h = (int)bh;
        float gw = side && nsmall ? bw + 40 + sw : bw;
        k->x = (int)(cx - gw * 0.5f);
        k->y = (int)y0;
        k->ink = mr_rgb_scale(g->p[p].color, 0.62f);
        for (int j = 0; j < nsmall; j++) {
            stk_t *q = &C->k[2 + j * 2 + p];
            q->used = true;
            q->w = (int)sw;
            q->h = (int)sh;
            if (side) {
                q->x = (int)(k->x + bw + 40);
                q->y = (int)(y0 + (nsmall == 1 ? (bh - sh) * 0.5f : j * (sh + 64)));
            } else {
                float gx = nsmall == 1 ? cx : cx + (j ? 0.5f : -0.5f) * (sw + 26);
                q->x = (int)(gx - sw * 0.5f);
                q->y = (int)(y0 + bh + 70);
            }
            q->ink = 0x2A2A2A;
        }
    }
    if (!mr_cv_new(&C->bg, s->w, s->h)) return;
    stand_draw(s);
    mr_cv_obj(s->obj, &C->bg);
    mr_img_new(&C->back[0], (int)bw, (int)bh);
    card_draw(&C->back[0], "", 0, true);
    mr_img_new(&C->back[1], (int)sw, (int)sh);
    card_draw(&C->back[1], "", 0, true);
    for (int i = 0; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->used) continue;
        mr_img_new(&k->im_stat, k->w, k->h);
        mr_img_new(&k->im_flip, k->w, k->h);
        k->stat = mr_img_obj(s->obj, &k->im_stat);
        lv_obj_set_pos(k->stat, k->x, k->y);
        k->flip = mr_img_obj(s->obj, &k->im_flip);
        lv_obj_set_pos(k->flip, k->x, k->y);
        lv_image_set_pivot(k->flip, k->w / 2, 0);
        lv_obj_add_flag(k->flip, LV_OBJ_FLAG_HIDDEN);
        mr_img_t *bk = &C->back[k->big ? 0 : 1];
        k->back = mr_img_obj(s->obj, bk);
        lv_obj_set_pos(k->back, k->x, k->y - k->h);
        lv_image_set_pivot(k->back, k->w / 2, k->h);
        lv_obj_add_flag(k->back, LV_OBJ_FLAG_HIDDEN);
        k->shown[0] = 0;
    }
    rings_draw(s, s->obj);
    /* names over the big stacks, captions under the small ones */
    for (int p = 0; p < 2; p++) {
        stk_t *k = &C->k[p];
        lv_obj_t *l = mr_label(s->obj, aos_font_title, 0xFFFFFF, g->p[p].name);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, k->w + 40, lv_font_get_line_height(aos_font_title));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, k->x - 20, k->y - 74);
        lv_obj_t *u = lv_obj_create(s->obj);
        mr_obj_plain(u);
        lv_obj_set_size(u, 60, 6);
        lv_obj_set_style_bg_color(u, lv_color_hex(g->p[p].color), 0);
        lv_obj_set_style_bg_opa(u, 255, 0);
        lv_obj_set_style_radius(u, 3, 0);
        lv_obj_set_pos(u, k->x + k->w / 2 - 30, k->y - 28);
        mr_rbtn(s, k->x + k->w / 2 - 50, (int)(s->h - 110), 100, 70, LV_SYMBOL_MINUS, 0xC0102A44, 0xFFFFFF, car_btn, p);
    }
    for (int i = 2; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->used || (i & 1)) continue;
        const char *cap = C->mode == MR_CAR_PING ? _("Games") : i < 4 ? _("Games") : _("Sets");
        for (int p = 0; p < 2; p++) {
            stk_t *q = &C->k[i + p];
            lv_obj_t *l = mr_label(s->obj, aos_font_caption, 0xDCE8F6, cap);
            lv_obj_update_layout(l);
            lv_obj_set_pos(l, q->x + q->w / 2 - lv_obj_get_width(l) / 2, q->y + q->h + 18);
        }
    }
}

static void sfree(mr_scene_t *s)
{
    car_t *C = ST;
    if (!C) return;
    mr_cv_free(&C->bg);
    mr_img_free(&C->back[0]);
    mr_img_free(&C->back[1]);
    for (int i = 0; i < NSTK; i++) {
        mr_img_free(&C->k[i].im_stat);
        mr_img_free(&C->k[i].im_flip);
    }
}

/* --- the flip ------------------------------------------------------------------------------------ */

static void flip_start(stk_t *k, const char *to, bool rev)
{
    if (k->on) {
        /* one still turning: finish it at once */
        stack_show(k, k->pend);
        k->on = false;
        lv_obj_add_flag(k->flip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(k->back, LV_OBJ_FLAG_HIDDEN);
    }
    snprintf(k->pend, sizeof k->pend, "%s", to);
    k->on = true;
    k->rev = rev;
    k->t = 0;
    if (!rev) {
        /* the old card goes over; the new one is under it already */
        card_draw(&k->im_flip, k->shown, k->ink, false);
        stack_show(k, to);
        snprintf(k->shown, sizeof k->shown, "%s", to);
    } else {
        /* the new card comes back over the top onto the old one */
        card_draw(&k->im_flip, to, k->ink, false);
    }
    lv_image_cache_drop(&k->im_flip.dsc);
    mr_snd_play(MR_S_FLIP, k->big ? 0.9f : 0.6f, k->big ? 1.0f : 1.25f);
}

static void frame(mr_scene_t *s, float dt)
{
    car_t *C = ST;
    if (!C) return;
    for (int i = 0; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->on) continue;
        k->t += dt / FLIP_S;
        float t = mr_clampf(k->t, 0, 1);
        /* eased angle, 0 .. 270 degrees, about the rings */
        float e = t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
        float phi = (k->rev ? 1 - e : e) * 4.712389f;
        float c = cosf(phi), sn = sinf(phi);
        uint32_t sx = (uint32_t)(256 * (1.0f + 0.10f * sn));
        if (c > 0) {
            lv_obj_remove_flag(k->flip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(k->back, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_scale_y(k->flip, (uint32_t)mr_maxf(2, 256 * c));
            lv_image_set_scale_x(k->flip, sx);
            lv_obj_set_style_image_recolor(k->flip, lv_color_hex(0x000000), 0);
            lv_obj_set_style_image_recolor_opa(k->flip, (lv_opa_t)(110 * (1 - c)), 0);
        } else {
            lv_obj_add_flag(k->flip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(k->back, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_scale_y(k->back, (uint32_t)mr_maxf(2, 256 * -c));
            lv_image_set_scale_x(k->back, sx);
        }
        if (k->t >= 1) {
            k->on = false;
            lv_obj_add_flag(k->flip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(k->back, LV_OBJ_FLAG_HIDDEN);
            if (k->rev) stack_show(k, k->pend);
            mr_snd_play(MR_S_BEAN_SOFT, 0.25f, 0.5f);
        }
    }
}

static void sync(mr_scene_t *s, int why, int who)
{
    car_t *C = ST;
    if (!C) return;
    (void)who;
    char t[NSTK][6];
    texts(s, t);
    for (int i = 0; i < NSTK; i++) {
        stk_t *k = &C->k[i];
        if (!k->used) continue;
        if (why == MR_SYNC_LOAD || !k->shown[0]) {
            stack_show(k, t[i]);
            continue;
        }
        const char *now = k->on ? k->pend : k->shown;
        if (strcmp(now, t[i])) flip_start(k, t[i], why == MR_SYNC_UNDO);
    }
}

/* --- fingers ---------------------------------------------------------------------------------------- */

static void car_btn(mr_scene_t *s, int arg)
{
    car_t *C = ST;
    if (C->mode == MR_CAR_FREE) mr_add(s->g, arg, -1);
    else {
        /* in a ruled game the last point of that player goes back */
        mr_game_t *g = s->g;
        for (int i = g->nhist - 1; i >= 0; i--) {
            if (g->hist[i].who == arg) {
                if (i == g->nhist - 1) mr_undo(g);
                else mr_toast(_("Sólo se puede deshacer el último punto"));
                return;
            }
        }
    }
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    car_t *C = ST;
    if (!C) return;
    C->f[f].who = -1;
    for (int p = 0; p < 2; p++) {
        stk_t *k = &C->k[p];
        if (x >= k->x - 20 && x < k->x + k->w + 20 && y >= k->y - 40 && y < k->y + k->h + 30) C->f[f].who = p;
    }
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    car_t *C = ST;
    if (!C) return;
    int p = C->f[f].who;
    C->f[f].who = -1;
    if (p < 0 || x < -1000 || mr_A->fing[f].moved) return;
    (void)y;
    if (s->g->winner >= 0 && C->mode != MR_CAR_FREE) {
        mr_toast(_("El partido terminó: poné la cuenta en cero desde el menú"));
        return;
    }
    mr_point(s->g, p);
}

const mr_scene_ops_t mr_cartel_ops = { build, sfree, sync, frame, down, NULL, up, NULL };
