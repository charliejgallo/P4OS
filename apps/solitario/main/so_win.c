/*
 * SOLITARIO - the endings
 *
 * Three, to choose in the options (or one at random):
 *
 *   Rebote: the cards jump off the foundations one by one and bounce down
 *   the screen, leaving their trail. The trail is real: each card is
 *   stamped onto the felt's own buffer as it goes (so_stamp565) and the
 *   felt is never cleared, which is exactly how the old desktop ending got
 *   its look. Only the stamped rectangles are invalidated. When it is over
 *   the clean felt is copied back.
 *
 *   Fuegos artificiales: cards rise from below like rockets and burst at the
 *   top into small spinning cards and sparks that fall and fade. These are
 *   objects (lv_image scaled and turned, small round lv_obj), a pool of them
 *   reused, nothing stamped.
 *
 *   Espiral: the cards come out of the centre one after another, each along
 *   its own arm of a spiral, turning as they go and growing; stamped like
 *   the bounce (turned, with so_stamp565_rot), so the arms stay drawn and
 *   the felt ends up a galaxy of cards.
 *
 * A tap ends any of them; then the app shows what was won.
 */
#include "so_app.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_fonts.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { P_STAMP = 0, P_ROCKET, P_MINI, P_SPARK, P_SPIRAL };

static float frand(so_win_t *w)
{
    return (float)(so_rand(&w->rnd) & 0xFFFF) / 65535.f;
}

/* the felt's buffer was drawn on between these corners: repaint them */
static void felt_dirty(so_app_t *a, int x0, int y0, int x1, int y1)
{
    lv_area_t c;
    lv_obj_get_coords(a->v.felt, &c);
    lv_area_t r = { c.x1 + x0, c.y1 + y0, c.x1 + x1, c.y1 + y1 };
    lv_obj_invalidate_area(a->v.felt, &r);
}

static void stamp(so_app_t *a, int card, float x, float y)
{
    so_view_t *v = &a->v;
    so_img_t *img = so_view_face_img(a, card);
    if (!img->px) return;
    int ix = (int)x - v->art.pad, iy = (int)y - v->art.pad;
    so_stamp565(v->feltmem, v->W, v->H, img, ix, iy);
    felt_dirty(a, ix, iy, ix + img->w, iy + img->h);
    a->win.stamped = true;
}

static void stamp_rot(so_app_t *a, int card, float cx, float cy, float deg, float k)
{
    so_view_t *v = &a->v;
    so_img_t *img = so_view_face_img(a, card);
    if (!img->px) return;
    so_stamp565_rot(v->feltmem, v->W, v->H, img, cx, cy, deg, k);
    int e = (int)((float)(img->w + img->h) * 0.5f * k) + 2;
    felt_dirty(a, (int)cx - e, (int)cy - e, (int)cx + e, (int)cy + e);
    a->win.stamped = true;
}

/* The cards in the order they leave: the tops of the foundations in turn
 * (kings first), then whatever else is on the table. */
static void build_order(so_app_t *a)
{
    so_game_t *g = &a->g;
    so_win_t *w = &a->win;
    uint8_t left[SO_MAXPILES];
    for (int p = 0; p < g->npiles; p++) left[p] = g->pn[p];
    w->norder = 0;
    for (int pass = 0; pass < 2; pass++) {
        bool any = true;
        while (any) {
            any = false;
            for (int p = 0; p < g->npiles; p++) {
                bool home = g->ptype[p] == SO_P_FOUND || g->ptype[p] == SO_P_DISCARD;
                if (pass == 0 ? !home : home) continue;
                if (!left[p]) continue;
                w->order[w->norder++] = g->pc[p][--left[p]];
                any = true;
            }
        }
    }
}

static int part_new(so_win_t *w, int type)
{
    for (int i = 0; i < SO_WIN_MAXP; i++) {
        if (w->p[i].live) continue;
        so_part_t *p = &w->p[i];
        memset(p, 0, sizeof *p);
        p->live = true;
        p->type = (uint8_t)type;
        return i;
    }
    return -1;
}

static lv_obj_t *part_obj(so_app_t *a, int i, int type, int card)
{
    so_win_t *w = &a->win;
    if (w->obj[i]) {
        lv_obj_delete(w->obj[i]);
        w->obj[i] = NULL;
    }
    lv_obj_t *o;
    if (type == P_SPARK) {
        o = lv_obj_create(w->box);
        lv_obj_remove_style_all(o);
        static const uint32_t SPARK[6] = { 0xFFE27A, 0xFF7A5C, 0x7CE0FF, 0xB9FF7A, 0xFF9BE8, 0xFFFFFF };
        lv_obj_set_style_bg_color(o, lv_color_hex(SPARK[so_rand(&w->rnd) % 6]), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        int s = 6 + (int)(so_rand(&w->rnd) % 6);
        lv_obj_set_size(o, s, s);
    } else {
        o = lv_image_create(w->box);
        lv_image_set_src(o, so_view_face(a, card));
        lv_image_set_antialias(o, true);
        lv_image_set_pivot(o, a->v.art.cv_w / 2, a->v.art.cv_h / 2);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    w->obj[i] = o;
    return o;
}

static void part_kill(so_win_t *w, int i)
{
    w->p[i].live = false;
    if (w->obj[i]) {
        lv_obj_delete(w->obj[i]);
        w->obj[i] = NULL;
    }
}

void so_win_start(so_app_t *a)
{
    so_win_t *w = &a->win;
    so_view_t *v = &a->v;
    so_win_stop(a);
    w->kind = a->set.win_anim;
    w->rnd = (uint32_t)aos_hal_uptime_ms() * 2654435761u + 7u;
    if (w->kind == SO_WIN_RANDOM || w->kind >= SO_NWINS) {
        w->kind = (int)(so_rand(&w->rnd) % 3);
    }
    w->t = 0;
    w->next = 0;
    w->wait = 300;
    build_order(a);

    w->box = lv_obj_create(a->root);
    lv_obj_remove_style_all(w->box);
    lv_obj_set_size(w->box, v->W, v->H);
    lv_obj_remove_flag(w->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(w->box, LV_OBJ_FLAG_SCROLLABLE);

    if (w->kind == SO_WIN_SPIRAL) {
        so_view_hide_cards(a, true);
    }

    /* "you won", above it all while it plays */
    w->banner = lv_label_create(a->root);
    lv_label_set_text(w->banner, _("¡Ganaste!"));
    lv_obj_set_style_text_font(w->banner, &aos_inter_48, 0);
    lv_obj_set_style_text_color(w->banner, lv_color_hex(0xFFE9A8), 0);
    lv_obj_set_style_bg_color(w->banner, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(w->banner, 120, 0);
    lv_obj_set_style_radius(w->banner, 36, 0);
    lv_obj_set_style_pad_hor(w->banner, 40, 0);
    lv_obj_set_style_pad_ver(w->banner, 14, 0);
    lv_obj_remove_flag(w->banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(w->banner, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(w->banner, lv_pct(50), 0);
    lv_obj_align(w->banner, LV_ALIGN_CENTER, 0, v->land ? -v->H / 3 + 30 : -v->H / 3);
}

void so_win_stop(so_app_t *a)
{
    so_win_t *w = &a->win;
    for (int i = 0; i < SO_WIN_MAXP; i++) {
        w->p[i].live = false;
        w->obj[i] = NULL;               /* deleted with the box */
    }
    if (w->box) {
        lv_obj_delete(w->box);
        w->box = NULL;
    }
    if (w->banner) {
        lv_obj_delete(w->banner);
        w->banner = NULL;
    }
    if (w->stamped && a->v.feltmem && a->v.feltclean) {
        memcpy(a->v.feltmem, a->v.feltclean, (size_t)a->v.W * a->v.H * 2);
        lv_obj_invalidate(a->v.felt);
        w->stamped = false;
    }
    w->norder = 0;
}

/* --- the bounce ----------------------------------------------------------------------- */

static void bounce_launch(so_app_t *a, int card)
{
    so_win_t *w = &a->win;
    int i = part_new(w, P_STAMP);
    if (i < 0) return;
    so_part_t *p = &w->p[i];
    so_cv_t *c = &a->v.cv[card];
    p->card = (int16_t)card;
    p->x = p->lx = c->x;
    p->y = p->ly = c->y;
    float dir = (p->x + (float)a->v.cw * 0.5f < (float)a->v.W * 0.5f) ? 1.f : -1.f;
    if (frand(w) < 0.25f) dir = -dir;
    p->vx = dir * (160.f + frand(w) * 380.f);
    p->vy = -(frand(w) * 720.f);
    lv_obj_add_flag(a->v.card[card], LV_OBJ_FLAG_HIDDEN);
    stamp(a, card, p->x, p->y);
}

static bool bounce_frame(so_app_t *a, int dt)
{
    so_win_t *w = &a->win;
    so_view_t *v = &a->v;
    float floor_y = (float)(v->H - v->ch - 4);
    int live = 0;
    for (int i = 0; i < SO_WIN_MAXP; i++) {
        so_part_t *p = &w->p[i];
        if (!p->live) continue;
        live++;
        /* small steps, a stamp whenever it has gone far enough */
        for (int s = 0; s < dt; s += 6) {
            float h = (float)(dt - s < 6 ? dt - s : 6) / 1000.f;
            p->vy += 2300.f * h;
            p->x += p->vx * h;
            p->y += p->vy * h;
            if (p->y > floor_y) {
                p->y = floor_y;
                p->vy = -p->vy * 0.80f;
            }
            float dx = p->x - p->lx, dy = p->y - p->ly;
            if (dx * dx + dy * dy > 64.f) {
                stamp(a, p->card, p->x, p->y);
                p->lx = p->x;
                p->ly = p->y;
            }
        }
        if (p->x < -(float)v->cw - 10.f || p->x > (float)v->W + 10.f) {
            p->live = false;
        }
    }
    w->wait -= dt;
    if (w->next < w->norder && (live == 0 || (w->wait <= 0 && live < 3))) {
        bounce_launch(a, w->order[w->next++]);
        w->wait = 700;
    }
    return w->next < w->norder || live > 0;
}

/* --- fireworks ------------------------------------------------------------------------- */

static void rocket(so_app_t *a)
{
    so_win_t *w = &a->win;
    so_view_t *v = &a->v;
    int i = part_new(w, P_ROCKET);
    if (i < 0) return;
    so_part_t *p = &w->p[i];
    p->card = (int16_t)(so_rand(&w->rnd) % (uint32_t)a->g.ncards);
    p->x = (float)v->W * (0.2f + 0.6f * frand(w));
    p->y = (float)v->H + 20.f;
    p->vx = (frand(w) - 0.5f) * 160.f;
    p->vy = -(float)v->H * (1.15f + 0.25f * frand(w));
    p->vr = (frand(w) - 0.5f) * 500.f;
    p->k = 0.55f;
    lv_obj_t *o = part_obj(a, i, P_ROCKET, p->card);
    lv_image_set_scale(o, (uint32_t)(256.f * p->k));
}

static void burst(so_app_t *a, const so_part_t *r)
{
    so_win_t *w = &a->win;
    int minis = 9, sparks = 14;
    for (int k = 0; k < minis + sparks; k++) {
        bool mini = k < minis;
        int i = part_new(w, mini ? P_MINI : P_SPARK);
        if (i < 0) return;
        so_part_t *p = &w->p[i];
        float ang = ((float)k + frand(w) * 0.6f) * 6.2831853f / (float)(mini ? minis : sparks);
        float sp = (mini ? 260.f : 420.f) * (0.6f + 0.6f * frand(w));
        p->x = r->x;
        p->y = r->y;
        p->vx = cosf(ang) * sp;
        p->vy = sinf(ang) * sp - 120.f;
        p->vr = (frand(w) - 0.5f) * 900.f;
        p->k = mini ? 0.30f + 0.08f * frand(w) : 1.f;
        p->life = (int16_t)(1300 + (int)(frand(w) * 500.f));
        p->card = mini ? (int16_t)(so_rand(&w->rnd) % (uint32_t)a->g.ncards) : 0;
        lv_obj_t *o = part_obj(a, i, mini ? P_MINI : P_SPARK, p->card);
        if (mini) lv_image_set_scale(o, (uint32_t)(256.f * p->k));
    }
    so_beep(a, 3);
}

static bool fireworks_frame(so_app_t *a, int dt)
{
    so_win_t *w = &a->win;
    so_view_t *v = &a->v;
    float h = (float)dt / 1000.f;
    int live = 0;
    for (int i = 0; i < SO_WIN_MAXP; i++) {
        so_part_t *p = &w->p[i];
        if (!p->live) continue;
        live++;
        lv_obj_t *o = w->obj[i];
        p->age = (int16_t)(p->age + dt);
        float grav = p->type == P_ROCKET ? 700.f : 520.f;
        p->vy += grav * h;
        if (p->type != P_ROCKET) {
            p->vx *= 0.985f;
            p->vy *= 0.985f;
        }
        p->x += p->vx * h;
        p->y += p->vy * h;
        p->rot += p->vr * h;
        if (p->type == P_ROCKET && p->vy > -60.f) {
            burst(a, p);
            part_kill(w, i);
            continue;
        }
        if (p->type != P_ROCKET && p->age > p->life) {
            part_kill(w, i);
            continue;
        }
        if (!o) continue;
        if (p->type == P_SPARK) {
            lv_obj_set_pos(o, (int32_t)p->x, (int32_t)p->y);
        } else {
            lv_obj_set_pos(o, (int32_t)p->x - v->art.cv_w / 2, (int32_t)p->y - v->art.cv_h / 2);
            lv_image_set_rotation(o, (int32_t)(p->rot * 10.f));
        }
        if (p->type != P_ROCKET) {
            float f = 1.f - (float)p->age / (float)p->life;
            lv_obj_set_style_opa(o, (lv_opa_t)(f < 0 ? 0 : f * 255.f), 0);
        }
    }
    w->wait -= dt;
    if (w->next < 14 && w->wait <= 0) {
        rocket(a);
        w->next++;
        w->wait = 650 + (int32_t)(frand(w) * 500.f);
    }
    return w->next < 14 || live > 0;
}

/* --- the spiral ------------------------------------------------------------------------- */

static bool spiral_frame(so_app_t *a, int dt)
{
    so_win_t *w = &a->win;
    so_view_t *v = &a->v;
    float cx = (float)v->W * 0.5f, cy = (float)v->H * 0.5f;
    float rmax = sqrtf(cx * cx + cy * cy) + (float)v->ch;
    int live = 0;
    for (int i = 0; i < SO_WIN_MAXP; i++) {
        so_part_t *p = &w->p[i];
        if (!p->live) continue;
        live++;
        for (int s = 0; s < dt; s += 10) {
            float h = (float)(dt - s < 10 ? dt - s : 10) / 1000.f;
            p->k += h * 0.55f;                  /* the radius grows, the arm turns */
            float r = 12.f + p->k * p->k * rmax * 0.55f;
            float th = p->rot + p->k * 4.2f;
            p->x = cx + cosf(th) * r;
            p->y = cy + sinf(th) * r;
            float dx = p->x - p->lx, dy = p->y - p->ly;
            float scale = 0.35f + 0.65f * (p->k > 1.f ? 1.f : p->k);
            float gap = (float)v->cw * 0.42f * scale;
            if (dx * dx + dy * dy > gap * gap) {
                stamp_rot(a, p->card, p->x, p->y, th * 57.29578f + 90.f, scale);
                p->lx = p->x;
                p->ly = p->y;
            }
            if (r > rmax) {
                p->live = false;
                break;
            }
        }
    }
    w->wait -= dt;
    if (w->next < w->norder && w->wait <= 0) {
        int i = part_new(w, P_SPIRAL);
        if (i >= 0) {
            so_part_t *p = &w->p[i];
            p->card = w->order[w->next];
            p->rot = (float)w->next * 2.39996f;     /* the golden angle */
            p->x = p->lx = cx;
            p->y = p->ly = cy;
            w->next++;
        }
        w->wait = 150;
    }
    return w->next < w->norder || live > 0;
}

bool so_win_frame(so_app_t *a, int dt)
{
    so_win_t *w = &a->win;
    if (!w->box) {
        return false;
    }
    w->t += (uint32_t)dt;
    /* the banner breathes */
    if (w->banner) {
        float q = sinf((float)w->t * 0.004f);
        lv_obj_set_style_transform_scale_x(w->banner, (int32_t)(256.f + 10.f * q), 0);
        lv_obj_set_style_transform_scale_y(w->banner, (int32_t)(256.f + 10.f * q), 0);
    }
    switch (w->kind) {
    case SO_WIN_FIREWORKS: return fireworks_frame(a, dt);
    case SO_WIN_SPIRAL:    return spiral_frame(a, dt);
    default:               return bounce_frame(a, dt);
    }
}
