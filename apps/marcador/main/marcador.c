/*
 * P4OS - Marcador
 *
 * A scorekeeper for table games and sports, with seven kinds of object to
 * keep the score on, each drawn and played as the real thing: matches on a
 * green felt (truco), beans dragged from a heap on a wooden table, hand
 * tally counters, a stadium board with lamps and a clock, the flip card of
 * table tennis and padel, an abacus and a slate with chalk.
 *
 * This file is the app around them: its life, the list of saved games, the
 * form for a new one, the bar over a game and its panels (history, menu,
 * keypad, the winner), and the fingers. The scenes read the touch panel raw
 * (aos_touch_points_n), so two people can each score with a finger at once;
 * the bar and the panels are ordinary LVGL buttons, and a finger that lands
 * on them is left to LVGL.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "mr_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_H      84
#define LIST_MAX   48
#define RBTN_MAX   48
#define C_BG       0x111214
#define C_CARD     0x1F2124
#define C_CARD2    0x2B2E33
#define C_LINE     0x3A3D42
#define C_DIM      0x8E8E93
#define C_ACC      0xF2B035

mr_app_t *mr_A;

static const mr_scene_ops_t *const OPS[MR_T_N] = { &mr_pano_ops,    &mr_porotos_ops, &mr_cuenta_ops, &mr_estadio_ops,
                                                   &mr_cartel_ops,  &mr_abaco_ops,   &mr_tiza_ops };

static const char *const TYPE_NAME[MR_T_N] = { N_("Paño y fósforos"), N_("Porotos"), N_("Cuentaganado"),
                                               N_("Tablero de estadio"), N_("Cartel de mesa"), N_("Ábaco"),
                                               N_("Pizarrón") };
static const char *const TYPE_DESC[MR_T_N] = {
    N_("Truco y juegos de mesa: cuadrados de cinco fósforos, malas y buenas"),
    N_("Un montón en la mesa y cada uno arrastra los suyos"),
    N_("El contador de mano: lo apretás y suma"),
    N_("Lámparas, reloj de juego, tiempos y bocina"),
    N_("Los números que se dan vuelta, para ping-pong y pádel"),
    N_("Cuentas de madera que se corren de a una"),
    N_("Palitos o cuentas con tiza; el borrador deshace"),
};

const char *mr_type_name(int type)
{
    return type >= 0 && type < MR_T_N ? _(TYPE_NAME[type]) : "";
}

uint32_t mr_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static void show_list(void);
static void show_new(void);
static void show_setup(void);
static void open_game(mr_game_t *g);
static void close_sheet(void);
static void confetti_frame(float dt);
static void on_close(lv_event_t *e);

/* --- small widgets ------------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *mr_pill(lv_obj_t *parent, const char *txt, int w, int h, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(mr_rgb_scale(bg, 0.72f)), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_x(b, 246, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(b, 246, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, h / 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (txt) {
        lv_obj_t *l = mr_label(b, aos_font_body, (bg & 0xFFFFFF) > 0xB0B0B0 ? 0x101010 : 0xFFFFFF, txt);
        lv_obj_center(l);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void mr_toast(const char *msg)
{
    aos_ui_toast(msg, 1600);
}

static lv_obj_t *col(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    return c;
}

static lv_obj_t *row(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* --- buttons inside scenes ------------------------------------------------------------ */

typedef struct {
    lv_obj_t    *o;
    int          x, y, w, h;
    mr_rbtn_cb_t cb;
    int          arg;
    uint8_t      finger;
} rbtn_t;

static rbtn_t RB[RBTN_MAX];
static int    n_rb;

static void rbtn_forget(void)
{
    n_rb = 0;
    memset(RB, 0, sizeof RB);
}

lv_obj_t *mr_rbtn(mr_scene_t *s, int x, int y, int w, int h, const char *txt, uint32_t bg, uint32_t fg,
                  mr_rbtn_cb_t cb, int arg)
{
    if (n_rb >= RBTN_MAX) return NULL;
    lv_obj_t *b = box(s->obj, w, h, bg, h / 2 < 22 ? h / 2 : 22);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_opa(b, (lv_opa_t)((bg >> 24) ? (bg >> 24) : 255), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(mr_rgb_scale(bg & 0xFFFFFF, 1.45f)), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(b, 40, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_translate_y(b, 2, LV_STATE_PRESSED);
    if (txt) {
        lv_obj_t *l = mr_label(b, aos_font_body, fg, txt);
        lv_obj_center(l);
    }
    rbtn_t *r = &RB[n_rb++];
    r->o = b;
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->cb = cb;
    r->arg = arg;
    return b;
}

void mr_rbtn_move(lv_obj_t *o, int x, int y)
{
    for (int i = 0; i < n_rb; i++) {
        if (RB[i].o == o) {
            RB[i].x = x;
            RB[i].y = y;
            lv_obj_set_pos(o, x, y);
        }
    }
}

void mr_rbtn_hide(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static int rbtn_hit(float x, float y)
{
    for (int i = n_rb - 1; i >= 0; i--) {
        rbtn_t *r = &RB[i];
        if (!r->o || lv_obj_has_flag(r->o, LV_OBJ_FLAG_HIDDEN)) continue;
        if (x >= r->x - 6 && x < r->x + r->w + 6 && y >= r->y - 6 && y < r->y + r->h + 6) return i;
    }
    return -1;
}

/* --- fingers ---------------------------------------------------------------------------- */

static int8_t f_btn[MR_FINGERS];      /* the scene button a finger holds, or -1 */

static void fingers_release(void)
{
    mr_app_t *a = mr_A;
    for (int i = 0; i < MR_FINGERS; i++) {
        if (a->fing[i].id) {
            if (f_btn[i] >= 0) lv_obj_remove_state(RB[f_btn[i]].o, LV_STATE_PRESSED);
            else if (a->scene_on && a->scene.ops->up) a->scene.ops->up(&a->scene, i, -1e4f, -1e4f);
        }
        a->fing[i].id = 0;
        f_btn[i] = -1;
    }
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    int n = aos_touch_points_n(pts, AOS_TOUCH_MAX);
    (void)n;
    for (int i = 0; i < MR_FINGERS && i < AOS_TOUCH_MAX; i++) a->stale[i] = pts[i].down ? pts[i].id : 0;
}

static void fingers_poll(void)
{
    mr_app_t *a = mr_A;
    if (!a->scene_on) return;
    bool covered = a->sheet || a->kb || (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (covered != a->covered) {
        a->covered = covered;
        fingers_release();
    }
    if (covered) return;
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    /* a finger that was down when the scene came (the tap that opened it) is not ours */
    for (int s = 0; s < MR_FINGERS; s++) {
        bool there = false;
        for (int i = 0; i < AOS_TOUCH_MAX; i++)
            if (pts[i].down && pts[i].id == a->stale[s]) there = true;
        if (!there) a->stale[s] = 0;
    }
    lv_area_t org;
    lv_obj_get_coords(a->root, &org);
    for (int i = 0; i < MR_FINGERS && i < AOS_TOUCH_MAX; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        for (int s = 0; s < MR_FINGERS && id; s++)
            if (a->stale[s] == id) id = 0;
        float x = pts[i].x - (float)org.x1, y = pts[i].y - (float)org.y1;
        mr_finger_t *f = &a->fing[i];
        if (id != f->id) {
            if (f->id) {
                /* lifted */
                if (f_btn[i] >= 0) {
                    rbtn_t *r = &RB[f_btn[i]];
                    lv_obj_remove_state(r->o, LV_STATE_PRESSED);
                    if (f->x >= r->x - 20 && f->x < r->x + r->w + 20 && f->y >= r->y - 20 && f->y < r->y + r->h + 20)
                        r->cb(&a->scene, r->arg);
                } else if (a->scene.ops->up) {
                    a->scene.ops->up(&a->scene, i, f->x, f->y);
                }
                f_btn[i] = -1;
            }
            f->id = id;
            f_btn[i] = -1;
            if (id) {
                /* the bar is LVGL's */
                if (y < BAR_H && a->bar) {
                    a->stale[i] = id;
                    f->id = 0;
                    continue;
                }
                f->x0 = f->x = x;
                f->y0 = f->y = y;
                f->t0 = mr_ms();
                f->moved = false;
                int b = rbtn_hit(x, y);
                if (b >= 0) {
                    f_btn[i] = (int8_t)b;
                    lv_obj_add_state(RB[b].o, LV_STATE_PRESSED);
                    mr_snd_play(MR_S_TAP, 0.6f, 1.0f);
                } else if (a->scene.ops->down) {
                    a->scene.ops->down(&a->scene, i, x, y);
                }
            }
        } else if (id && (x != f->x || y != f->y)) {
            f->x = x;
            f->y = y;
            float dx = x - f->x0, dy = y - f->y0;
            if (dx * dx + dy * dy > 18 * 18) f->moved = true;
            if (f_btn[i] < 0 && a->scene.ops->move) a->scene.ops->move(&a->scene, i, x, y);
        }
    }
}

/* --- the frame ---------------------------------------------------------------------------- */

void mr_set_dirty(void)
{
    if (mr_A) mr_A->save_due = mr_ms() + 1500;
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    mr_app_t *a = mr_A;
    if (!a) return;
    uint32_t now = mr_ms();
    float dt = (float)(now - a->last_ms) / 1000.0f;
    a->last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0) dt = 0;
    fingers_poll();
    confetti_frame(dt);
    if (a->scene_on && a->scene.ops->frame) a->scene.ops->frame(&a->scene, dt);
    mr_snd_pump();
    if (now - a->tick_ms >= 200) {
        a->tick_ms = now;
        if (a->scene_on && a->scene.ops->tick) a->scene.ops->tick(&a->scene);
    }
    if (a->save_due && (int32_t)(now - a->save_due) >= 0) {
        a->save_due = 0;
        if (a->g) mr_save(a->g);
    }
}

/* --- screens ----------------------------------------------------------------------------- */

static void clear_root(void)
{
    mr_app_t *a = mr_A;
    if (a->kb) a->kb = NULL;
    a->sheet = NULL;
    if (a->scene_on) {
        if (a->scene.ops->free) a->scene.ops->free(&a->scene);
        a->scene_on = false;
        mr_free(a->scene.st);
        a->scene.st = NULL;
    }
    rbtn_forget();
    a->bar = a->bar_title = a->bar_undo = NULL;
    lv_obj_clean(a->root);
    lv_obj_set_style_bg_color(a->root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(a->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->root, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *header(const char *title, lv_event_cb_t back_cb)
{
    mr_app_t *a = mr_A;
    lv_obj_t *h = box(a->root, a->w, BAR_H, C_BG, 0);
    if (back_cb) {
        lv_obj_t *b = mr_pill(h, LV_SYMBOL_LEFT, 72, 60, C_CARD2, back_cb, NULL);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 16, 0);
    }
    lv_obj_t *l = mr_label(h, aos_font_title, 0xFFFFFF, title);
    lv_obj_align(l, back_cb ? LV_ALIGN_LEFT_MID : LV_ALIGN_LEFT_MID, back_cb ? 104 : 28, 0);
    return h;
}

/* ===== the list of games ===== */

typedef struct {
    uint32_t  ids[LIST_MAX];
    int       n;
    mr_img_t  thumb[MR_T_N];
    bool      thumbs;
    int       armed;           /* the card whose trash was touched once */
} list_t;

static list_t L;

static void thumbs_make(void)
{
    if (L.thumbs) return;
    for (int t = 0; t < MR_T_N; t++) {
        if (mr_img_new(&L.thumb[t], 112, 112)) mr_thumb(&L.thumb[t], t);
    }
    L.thumbs = true;
}

static void thumbs_free(void)
{
    for (int t = 0; t < MR_T_N; t++) mr_img_free(&L.thumb[t]);
    L.thumbs = false;
}

static void on_new(lv_event_t *e)
{
    (void)e;
    mr_snd_play(MR_S_TAP, 0.7f, 1.0f);
    show_new();
}

static void on_card(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    mr_game_t *g = mr_game_new(0);
    if (!g) return;
    if (!mr_load(g, id)) {
        mr_game_free(g);
        mr_toast(_("No se pudo abrir la partida"));
        return;
    }
    open_game(g);
}

static void on_trash(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (L.armed != i) {
        L.armed = i;
        lv_obj_t *b = lv_event_get_target_obj(e);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xC0392B), 0);
        mr_toast(_("Tocá otra vez para borrarla"));
        return;
    }
    mr_delete(L.ids[i]);
    L.armed = -1;
    show_list();
}

static void game_summary(const mr_game_t *g, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < g->np && k + 8 < n; i++) {
        char sc[24];
        if (g->type == MR_T_CARTEL && g->opt[MR_CAR_MODE] == MR_CAR_PADEL)
            snprintf(sc, sizeof sc, "%d-%d", g->p[i].aux[1], g->p[i].aux[0]);
        else if (g->type == MR_T_CARTEL && g->opt[MR_CAR_MODE] == MR_CAR_PING)
            snprintf(sc, sizeof sc, "%d (%ld)", g->p[i].aux[1], (long)g->p[i].score);
        else snprintf(sc, sizeof sc, "%ld", (long)g->p[i].score);
        int w = snprintf(out + k, n - k, "%s%.14s %s", i ? "  ·  " : "", g->p[i].name, sc);
        if (w < 0) break;
        k += (size_t)w;
        if (k >= n) {
            out[n - 1] = 0;
            break;
        }
    }
}

static void show_list(void)
{
    mr_app_t *a = mr_A;
    clear_root();
    a->screen = MR_SCR_LIST;
    if (a->g) {
        mr_save(a->g);
        mr_game_free(a->g);
        a->g = NULL;
    }
    aos_hal_pref_set_i32("mr_open", 0);
    thumbs_make();
    L.armed = -1;
    header(_("Marcador"), NULL);
    lv_obj_t *nb = mr_pill(a->root, NULL, 220, 64, C_ACC, on_new, NULL);
    lv_obj_t *nl = mr_label(nb, aos_font_body, 0x1A1408, LV_SYMBOL_PLUS "  ");
    lv_label_set_text_fmt(nl, LV_SYMBOL_PLUS "  %s", _("Nueva"));
    lv_obj_center(nl);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -20, 10);

    L.n = mr_list(L.ids, LIST_MAX);
    int cw = a->land ? (a->w - 72) / 2 : a->w - 40;
    lv_obj_t *c = col(a->root, a->w - 40 + (a->land ? 12 : 0), a->h - BAR_H - 10);
    lv_obj_set_pos(c, 20, BAR_H + 6);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (a->land) {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(c, 12, 0);
    }
    lv_obj_set_style_pad_bottom(c, 30, 0);
    if (!L.n) {
        lv_obj_t *e = box(c, a->w - 40, a->land ? 420 : 560, C_BG, 0);
        lv_obj_set_flex_flow(e, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(e, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(e, 18, 0);
        lv_obj_t *r = row(e, 4 * 120, 120);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int t = 0; t < 4; t++) mr_img_obj(r, &L.thumb[(t * 2) % MR_T_N]);
        lv_obj_t *t1 = mr_label(e, aos_font_title, 0xFFFFFF, _("Todavía no hay partidas"));
        (void)t1;
        lv_obj_t *t2 = mr_label(e, aos_font_body, C_DIM, _("Tocá Nueva y elegí con qué llevar la cuenta"));
        lv_label_set_long_mode(t2, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(t2, a->w - 120);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    mr_game_t *g = mr_game_new(0);
    if (!g) return;
    for (int i = 0; i < L.n; i++) {
        if (!mr_load(g, L.ids[i])) continue;
        lv_obj_t *card = lv_button_create(c);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, cw, 136);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD2), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_add_event_cb(card, on_card, LV_EVENT_CLICKED, (void *)(uintptr_t)L.ids[i]);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_t *im = mr_img_obj(card, &L.thumb[g->type < MR_T_N ? g->type : 0]);
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *t = mr_label(card, aos_font_body, 0xFFFFFF, g->name);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(t, cw - 236, lv_font_get_line_height(aos_font_body));
        lv_obj_set_pos(t, 140, 16);
        char sum[160];
        game_summary(g, sum, sizeof sum);
        lv_obj_t *s = mr_label(card, aos_font_small, 0xD8D8DC, sum);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s, cw - 236, lv_font_get_line_height(aos_font_small));
        lv_obj_set_pos(s, 140, 56);
        char sub[96];
        int nh = 0;
        for (int k = 0; k < g->nhist; k++)
            if (g->hist[k].kind != MR_H_RESET) nh++;
        if (g->winner >= 0 && g->winner < g->np)
            snprintf(sub, sizeof sub, _("%s · ganó %.20s"), mr_type_name(g->type), g->p[g->winner].name);
        else if (nh == 1) snprintf(sub, sizeof sub, _("%s · 1 jugada"), mr_type_name(g->type));
        else snprintf(sub, sizeof sub, _("%s · %d jugadas"), mr_type_name(g->type), nh);
        lv_obj_t *s2 = mr_label(card, aos_font_caption, C_DIM, sub);
        lv_label_set_long_mode(s2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s2, cw - 236, lv_font_get_line_height(aos_font_caption));
        lv_obj_set_pos(s2, 140, 94);
        lv_obj_t *tr = mr_pill(card, LV_SYMBOL_TRASH, 72, 72, C_CARD2, on_trash, (void *)(intptr_t)i);
        lv_obj_align(tr, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    mr_game_free(g);
}

/* ===== a new game: the kinds ===== */

static void on_list_back(lv_event_t *e)
{
    (void)e;
    show_list();
}

static void on_kind(lv_event_t *e)
{
    mr_app_t *a = mr_A;
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    mr_snd_play(MR_S_TAP, 0.7f, 1.0f);
    if (!a->draft) a->draft = mr_game_new(t);
    if (!a->draft) return;
    mr_hist_t *h = a->draft->hist;
    memset(a->draft, 0, sizeof *a->draft);
    a->draft->hist = h;
    a->draft->winner = -1;
    a->draft->period = 1;
    mr_game_defaults(a->draft, t, mr_type_minp(t) == 1 ? (t == MR_T_CUENTA ? 2 : 2) : 2);
    a->draft_edit = false;
    show_setup();
}

static void show_new(void)
{
    mr_app_t *a = mr_A;
    clear_root();
    a->screen = MR_SCR_NEW;
    thumbs_make();
    header(_("¿Con qué llevan la cuenta?"), on_list_back);
    int cols = a->land ? 2 : 1;
    int cw = (a->w - 40 - 12 * (cols - 1)) / cols;
    lv_obj_t *c = col(a->root, a->w - 40 + 12, a->h - BAR_H - 8);
    lv_obj_set_pos(c, 20, BAR_H + 4);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_style_pad_bottom(c, 30, 0);
    for (int t = 0; t < MR_T_N; t++) {
        lv_obj_t *card = lv_button_create(c);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, cw, 136);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD2), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(card, on_kind, LV_EVENT_CLICKED, (void *)(intptr_t)t);
        lv_obj_t *im = mr_img_obj(card, &L.thumb[t]);
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *n = mr_label(card, aos_font_body, 0xFFFFFF, _(TYPE_NAME[t]));
        lv_obj_set_pos(n, 140, 18);
        lv_obj_t *d = mr_label(card, aos_font_small, C_DIM, _(TYPE_DESC[t]));
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(d, cw - 160);
        lv_obj_set_pos(d, 140, 58);
    }
}

/* ===== the form: players, target, rules ===== */

typedef struct {
    lv_obj_t *ta[MR_MAXP];
    lv_obj_t *name_ta;
    lv_obj_t *form;
} form_t;

static form_t F;

static void kb_close(void)
{
    mr_app_t *a = mr_A;
    if (!a->kb) return;
    lv_obj_t *kb = a->kb;
    a->kb = NULL;
    lv_obj_delete_async(kb);
    if (F.form) lv_obj_set_height(F.form, a->h - BAR_H - 120);
}

static void kb_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) kb_close();
}

static void read_names(void)
{
    mr_game_t *d = mr_A->draft;
    if (!d) return;
    for (int i = 0; i < d->np; i++) {
        if (!F.ta[i]) continue;
        const char *t = lv_textarea_get_text(F.ta[i]);
        if (t && t[0]) snprintf(d->p[i].name, MR_NAME, "%s", t);
    }
    if (F.name_ta) {
        const char *t = lv_textarea_get_text(F.name_ta);
        if (t && t[0]) snprintf(d->name, sizeof d->name, "%s", t);
    }
}

static void ta_focus(lv_event_t *e)
{
    mr_app_t *a = mr_A;
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (!a->kb) {
        a->kb = lv_keyboard_create(a->root);
        lv_obj_set_size(a->kb, a->w, a->land ? a->h / 2 : 420);
        lv_obj_align(a->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(a->kb, kb_event, LV_EVENT_ALL, NULL);
        if (F.form) lv_obj_set_height(F.form, a->h - BAR_H - 120 - lv_obj_get_height(a->kb) + 110);
    }
    lv_keyboard_set_textarea(a->kb, ta);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

static lv_obj_t *field(lv_obj_t *parent, int w, const char *txt, int maxlen)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, 64);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)maxlen);
    lv_textarea_set_text(ta, txt);
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_pad_top(ta, 14, 0);
    lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void on_color(lv_event_t *e)
{
    mr_game_t *d = mr_A->draft;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int k = 0;
    for (int j = 0; j < 10; j++)
        if (mr_palette[j] == d->p[i].color) k = j;
    d->p[i].color = mr_palette[(k + 1) % 10];
    lv_obj_t *b = lv_event_get_target_obj(e);
    lv_obj_set_style_bg_color(b, lv_color_hex(d->p[i].color), 0);
    mr_snd_play(MR_S_TAP, 0.6f, 1.2f);
}

static void on_np(lv_event_t *e)
{
    mr_game_t *d = mr_A->draft;
    int dlt = (int)(intptr_t)lv_event_get_user_data(e);
    read_names();
    int np = d->np + dlt;
    if (np < mr_type_minp(d->type) || np > mr_type_maxp(d->type)) return;
    if (mr_A->draft_edit && np < d->np) {
        /* a player who goes takes the score along */
        d->p[np].score = 0;
    }
    d->np = (uint8_t)np;
    show_setup();
}

typedef struct {
    int16_t slot;      /* -1 target, else opt index */
    int16_t value;     /* -2: ask with the keypad */
} chip_t;

static void num_target(int v, int who)
{
    (void)who;
    mr_game_t *d = mr_A->draft;
    if (!d) return;
    d->target = v < 0 ? 0 : v;
    show_setup();
}

static void num_start(int v, int who)
{
    (void)who;
    mr_game_t *d = mr_A->draft;
    if (!d) return;
    d->opt[MR_TIZA_START] = v < 1 ? 1 : v;
    show_setup();
}

static void on_chip(lv_event_t *e)
{
    mr_game_t *d = mr_A->draft;
    intptr_t k = (intptr_t)lv_event_get_user_data(e);
    int slot = (int)(k >> 16) - 1, val = (int)(int16_t)(k & 0xFFFF);
    read_names();
    mr_snd_play(MR_S_TAP, 0.6f, 1.1f);
    if (val == -2) {
        if (slot < 0) mr_ui_number(_("Hasta cuántos"), d->target, -1, num_target);
        else mr_ui_number(_("Desde cuánto"), d->opt[slot], -1, num_start);
        return;
    }
    if (slot < 0) d->target = val;
    else d->opt[slot] = val;
    if (d->type == MR_T_ESTADIO && slot == MR_EST_SPORT) {
        static const int MIN[5] = { 45, 10, 30, 15, 20 }, PER[5] = { 2, 4, 2, 4, 2 }, DOWN[5] = { 0, 1, 0, 1, 1 };
        d->opt[MR_EST_MIN] = MIN[val];
        d->opt[MR_EST_PER] = PER[val];
        d->opt[MR_EST_DOWN] = DOWN[val];
    }
    if (d->type == MR_T_CARTEL && slot == MR_CAR_MODE) d->opt[MR_CAR_BEST] = val == MR_CAR_PADEL ? 3 : 5;
    if (d->type == MR_T_TIZA && slot == MR_TIZA_SUB && val) d->target = 0;
    show_setup();
}

static void chips(lv_obj_t *parent, int w, const char *title, int slot, const int *vals, const char *const *labels,
                  int n, int cur)
{
    lv_obj_t *t = mr_label(parent, aos_font_small, C_DIM, title);
    (void)t;
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    bool any = false;
    for (int i = 0; i < n; i++) {
        bool on = vals[i] == cur;
        if (vals[i] == -2 && !any) {
            /* "Otro" lights when the value is none of the others */
            bool other = true;
            for (int j = 0; j < n; j++)
                if (vals[j] == cur) other = false;
            on = other;
        }
        if (on) any = true;
        char buf[40];
        if (vals[i] == -2 && on) snprintf(buf, sizeof buf, "%s: %d", labels[i], cur);
        else snprintf(buf, sizeof buf, "%s", labels[i]);
        lv_obj_t *b = mr_pill(r, buf, LV_SIZE_CONTENT, 60, on ? C_ACC : C_CARD2, on_chip,
                              (void *)(intptr_t)(((slot + 1) << 16) | (vals[i] & 0xFFFF)));
        lv_obj_set_style_pad_hor(b, 24, 0);
    }
}

static void on_start(lv_event_t *e)
{
    (void)e;
    mr_app_t *a = mr_A;
    mr_game_t *d = a->draft;
    if (!d) return;
    read_names();
    kb_close();
    if (a->draft_edit && a->g) {
        /* the open game takes the new players and rules, keeps its scores */
        mr_game_t *g = a->g;
        int old_target = g->target;
        g->np = d->np;
        memcpy(g->p, d->p, sizeof g->p);
        g->target = d->target;
        memcpy(g->opt, d->opt, sizeof g->opt);
        snprintf(g->name, sizeof g->name, "%s", d->name);
        if (old_target != g->target) {
            g->announced = false;
            g->winner = -1;
        }
        a->draft_edit = false;
        mr_save(g);
        mr_game_t *keep = g;
        a->g = NULL;
        open_game(keep);
        return;
    }
    mr_game_t *g = mr_game_new(d->type);
    if (!g) return;
    mr_hist_t *h = g->hist;
    memcpy(g, d, sizeof *g);
    g->hist = h;
    g->nhist = 0;
    g->id = mr_new_id();
    g->created = mr_now();
    g->winner = -1;
    g->period = 1;
    for (int i = 0; i < MR_MAXP; i++) {
        g->p[i].score = 0;
        g->p[i].aux[0] = g->p[i].aux[1] = 0;
        g->p[i].wins = 0;
    }
    if (g->type == MR_T_TIZA && g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && g->opt[MR_TIZA_SUB])
        for (int i = 0; i < MR_MAXP; i++) g->p[i].score = g->opt[MR_TIZA_START];
    mr_snd_play(MR_S_TAP, 0.8f, 0.9f);
    mr_save(g);
    open_game(g);
}

static void on_setup_back(lv_event_t *e)
{
    (void)e;
    mr_app_t *a = mr_A;
    kb_close();
    if (a->draft_edit && a->g) {
        mr_game_t *g = a->g;
        a->g = NULL;
        a->draft_edit = false;
        open_game(g);
        return;
    }
    show_new();
}

static void show_setup(void)
{
    mr_app_t *a = mr_A;
    mr_game_t *d = a->draft;
    lv_coord_t keep_scroll = F.form && a->screen == MR_SCR_SETUP ? lv_obj_get_scroll_y(F.form) : 0;
    clear_root();
    a->screen = MR_SCR_SETUP;
    memset(&F, 0, sizeof F);
    header(a->draft_edit ? _("Jugadores y reglas") : mr_type_name(d->type), on_setup_back);
    int fw = a->land ? a->w - 80 : a->w - 48;
    if (fw > 900) fw = 900;
    F.form = col(a->root, fw, a->h - BAR_H - 120);
    lv_obj_set_pos(F.form, (a->w - fw) / 2, BAR_H + 4);
    lv_obj_add_flag(F.form, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_bottom(F.form, 40, 0);

    mr_label(F.form, aos_font_small, C_DIM, _("Nombre de la partida"));
    F.name_ta = field(F.form, fw, d->name, 38);

    /* players */
    lv_obj_t *pr = row(F.form, fw, 70);
    lv_obj_t *pl = mr_label(pr, aos_font_body, 0xFFFFFF, "");
    lv_label_set_text_fmt(pl, d->type == MR_T_CUENTA ? _("Contadores: %d") : _("Jugadores: %d"), d->np);
    lv_obj_set_flex_grow(pl, 1);
    if (mr_type_minp(d->type) != mr_type_maxp(d->type)) {
        mr_pill(pr, LV_SYMBOL_MINUS, 84, 60, C_CARD2, on_np, (void *)(intptr_t)-1);
        mr_pill(pr, LV_SYMBOL_PLUS, 84, 60, C_CARD2, on_np, (void *)(intptr_t)1);
    }
    for (int i = 0; i < d->np; i++) {
        lv_obj_t *r = row(F.form, fw, 70);
        lv_obj_t *sw = mr_pill(r, NULL, 64, 64, d->p[i].color, on_color, (void *)(intptr_t)i);
        lv_obj_set_style_border_color(sw, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(sw, 3, 0);
        lv_obj_set_style_border_opa(sw, 120, 0);
        F.ta[i] = field(r, fw - 76, d->p[i].name, MR_NAME - 1);
    }

    /* the target and the kind's own rules */
    static const char *const NO_T = N_("Sin meta"), *const OTHER = N_("Otro");
    switch (d->type) {
    case MR_T_PANO: {
        static const int V[] = { 15, 18, 24, 30, 40, -2 };
        const char *Lb[] = { "15", "18", "24", "30", "40", _(OTHER) };
        chips(F.form, fw, _("Se juega a"), -1, V, Lb, 6, d->target);
        static const int S2[] = { 1, 0 };
        const char *Ls[] = { _("Malas y buenas"), _("De corrido") };
        chips(F.form, fw, _("Cómo se cuenta"), MR_PANO_SPLIT, S2, Ls, 2, d->opt[MR_PANO_SPLIT]);
        break;
    }
    case MR_T_POROTOS: {
        static const int V[] = { 15, 20, 30, 50, 0 };
        const char *Lb[] = { "15", "20", "30", "50", _(NO_T) };
        chips(F.form, fw, _("Se juega a"), -1, V, Lb, 5, d->target);
        static const int K[] = { 0, 1, 2, 3 };
        const char *Lk[] = { _("Blancos"), _("Pintos"), _("Negros"), _("Mezclados") };
        chips(F.form, fw, _("Porotos"), MR_POR_KIND, K, Lk, 4, d->opt[MR_POR_KIND]);
        break;
    }
    case MR_T_CUENTA: {
        static const int V[] = { 0, 10, 50, 100, -2 };
        const char *Lb[] = { _(NO_T), "10", "50", "100", _(OTHER) };
        chips(F.form, fw, _("Meta"), -1, V, Lb, 5, d->target);
        break;
    }
    case MR_T_ESTADIO: {
        static const int V[] = { 0, 1, 2, 3, 4 };
        const char *Lb[] = { _("Fútbol"), _("Básquet"), _("Handball"), _("Hockey"), _("Otro deporte") };
        chips(F.form, fw, _("Deporte"), MR_EST_SPORT, V, Lb, 5, d->opt[MR_EST_SPORT]);
        static const int M[] = { 8, 10, 12, 15, 20, 25, 30, 35, 40, 45 };
        const char *Lm[] = { "8", "10", "12", "15", "20", "25", "30", "35", "40", "45" };
        chips(F.form, fw, _("Minutos por tiempo"), MR_EST_MIN, M, Lm, 10, d->opt[MR_EST_MIN]);
        static const int P[] = { 1, 2, 3, 4 };
        const char *Lp[] = { "1", "2", "3", "4" };
        chips(F.form, fw, _("Tiempos"), MR_EST_PER, P, Lp, 4, d->opt[MR_EST_PER]);
        static const int D[] = { 0, 1 };
        const char *Ld[] = { _("Reloj para adelante"), _("Cuenta regresiva") };
        chips(F.form, fw, _("Reloj"), MR_EST_DOWN, D, Ld, 2, d->opt[MR_EST_DOWN]);
        break;
    }
    case MR_T_CARTEL: {
        static const int V[] = { MR_CAR_PING, MR_CAR_PADEL, MR_CAR_FREE };
        const char *Lb[] = { _("Ping-pong"), _("Pádel y tenis"), _("Libre") };
        chips(F.form, fw, _("Reglas"), MR_CAR_MODE, V, Lb, 3, d->opt[MR_CAR_MODE]);
        if (d->opt[MR_CAR_MODE] != MR_CAR_FREE) {
            static const int B[] = { 1, 3, 5, 7 };
            const char *Lb2[] = { "1", "3", "5", "7" };
            chips(F.form, fw, d->opt[MR_CAR_MODE] == MR_CAR_PADEL ? _("Al mejor de (sets)") : _("Al mejor de (games)"),
                  MR_CAR_BEST, B, Lb2, 4, d->opt[MR_CAR_BEST]);
        } else {
            static const int T[] = { 0, 11, 21, -2 };
            const char *Lt[] = { _(NO_T), "11", "21", _(OTHER) };
            chips(F.form, fw, _("Meta"), -1, T, Lt, 4, d->target);
        }
        break;
    }
    case MR_T_ABACO: {
        static const int V[] = { 0, 50, 100, 500, -2 };
        const char *Lb[] = { _(NO_T), "50", "100", "500", _(OTHER) };
        chips(F.form, fw, _("Meta"), -1, V, Lb, 5, d->target);
        break;
    }
    case MR_T_TIZA: {
        static const int V[] = { MR_TIZA_TALLY, MR_TIZA_TOTALS };
        const char *Lb[] = { _("Palitos"), _("Cuentas") };
        chips(F.form, fw, _("Qué se escribe"), MR_TIZA_MODE, V, Lb, 2, d->opt[MR_TIZA_MODE]);
        if (d->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS) {
            static const int S2[] = { 0, 1 };
            const char *Ls[] = { _("Sumando"), _("Restando") };
            chips(F.form, fw, _("Cada anotación"), MR_TIZA_SUB, S2, Ls, 2, d->opt[MR_TIZA_SUB]);
        }
        if (d->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && d->opt[MR_TIZA_SUB]) {
            static const int S3[] = { 101, 301, 501, -2 };
            const char *Ls3[] = { "101", "301", "501", _(OTHER) };
            chips(F.form, fw, _("Desde"), MR_TIZA_START, S3, Ls3, 4, d->opt[MR_TIZA_START]);
        } else {
            static const int T[] = { 0, 20, 100, 500, -2 };
            const char *Lt[] = { _(NO_T), "20", "100", "500", _(OTHER) };
            chips(F.form, fw, _("Meta"), -1, T, Lt, 5, d->target);
        }
        break;
    }
    default: break;
    }
    if (keep_scroll) {
        lv_obj_update_layout(F.form);
        lv_obj_scroll_to_y(F.form, keep_scroll, LV_ANIM_OFF);
    }

    lv_obj_t *go = mr_pill(a->root, a->draft_edit ? _("Listo") : _("Empezar"), a->land ? 360 : a->w - 48, 80,
                           C_ACC, on_start, NULL);
    lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/* ===== the game ===== */

static void on_bar_back(lv_event_t *e)
{
    (void)e;
    show_list();
}

static void on_undo(lv_event_t *e)
{
    (void)e;
    mr_app_t *a = mr_A;
    if (!a->g) return;
    if (mr_undo(a->g)) mr_snd_play(MR_S_UNDO, 0.7f, 1.0f);
    else mr_toast(_("No hay nada para deshacer"));
}

static void sheet_bg_click(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e)) close_sheet();
}

static void on_close(lv_event_t *e)
{
    (void)e;
    close_sheet();
}

static void close_sheet(void)
{
    mr_app_t *a = mr_A;
    if (!a->sheet) return;
    lv_obj_t *s = a->sheet;
    a->sheet = NULL;
    lv_obj_delete_async(s);
}

/* a dimmed layer over everything and a card on it */
static lv_obj_t *sheet_open(int w, int h, const char *title)
{
    mr_app_t *a = mr_A;
    close_sheet();
    lv_obj_t *dim = lv_obj_create(a->root);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, a->w, a->h);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(dim, 150, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dim, sheet_bg_click, LV_EVENT_CLICKED, NULL);
    a->sheet = dim;
    if (w > a->w - 32) w = a->w - 32;
    if (h > a->h - 32) h = a->h - 32;
    lv_obj_t *card = box(dim, w, h, C_CARD, 28);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(card);
    lv_obj_set_style_shadow_width(card, 40, 0);
    lv_obj_set_style_shadow_opa(card, 160, 0);
    /* the callers measure it right away */
    lv_obj_update_layout(card);
    if (title) {
        lv_obj_t *t = mr_label(card, aos_font_title, 0xFFFFFF, title);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 28, 22);
    }
    return card;
}

/* --- history --- */

static void hist_line(const mr_game_t *g, const mr_hist_t *h, int k, char *out, size_t n)
{
    unsigned m = h->t / 60, s = h->t % 60;
    if (h->kind == MR_H_RESET) {
        snprintf(out, n, "%2u:%02u   %s", m, s, _("Se reinició la cuenta"));
        return;
    }
    if (h->who < 0 || h->who >= g->np) {
        out[0] = 0;
        return;
    }
    /* the score after: the next entry's picture, or the score now */
    int32_t after = k + 1 < g->nhist ? g->hist[k + 1].before[h->who] : g->p[h->who].score;
    if (g->type == MR_T_CARTEL && g->opt[MR_CAR_MODE] != MR_CAR_FREE) {
        snprintf(out, n, "%2u:%02u   %s %.18s", m, s, _("Punto para"), g->p[h->who].name);
    } else if (h->kind == MR_H_MOVE) {
        snprintf(out, n, "%2u:%02u   %+d %.18s  " LV_SYMBOL_RIGHT " %ld", m, s, h->delta, g->p[h->who].name, (long)after);
    } else {
        snprintf(out, n, "%2u:%02u   %+d %.18s  " LV_SYMBOL_RIGHT " %ld", m, s, h->delta, g->p[h->who].name, (long)after);
    }
}

static void show_history(void);

static void on_hist_undo(lv_event_t *e)
{
    on_undo(e);
    show_history();     /* again, with the line gone */
}

static void show_history(void)
{
    mr_app_t *a = mr_A;
    mr_game_t *g = a->g;
    lv_obj_t *card = sheet_open(a->land ? 760 : a->w - 40, a->h - 120, _("Historial"));
    int cw = lv_obj_get_width(card);
    int chh = lv_obj_get_height(card);
    lv_obj_t *c = col(card, cw - 40, chh - 200);
    lv_obj_set_pos(c, 20, 90);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    int shown = 0;
    for (int k = g->nhist - 1; k >= 0 && shown < 200; k--) {
        char line[96];
        hist_line(g, &g->hist[k], k, line, sizeof line);
        if (!line[0]) continue;
        lv_obj_t *r = box(c, cw - 40, 52, shown & 1 ? C_CARD : C_CARD2, 12);
        uint32_t col_c = g->hist[k].who >= 0 && g->hist[k].who < g->np ? g->p[g->hist[k].who].color : C_DIM;
        lv_obj_t *dot = box(r, 14, 14, col_c, 7);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *l = mr_label(r, aos_font_small, 0xE8E8EA, line);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, cw - 110, lv_font_get_line_height(aos_font_small));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 40, 0);
        shown++;
    }
    if (!shown) {
        lv_obj_t *l = mr_label(c, aos_font_body, C_DIM, _("Todavía no se anotó nada"));
        (void)l;
    }
    lv_obj_t *u = mr_pill(card, _("Deshacer"), 260, 70, C_CARD2, on_hist_undo, NULL);
    lv_obj_align(u, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_t *ok = mr_pill(card, _("Cerrar"), 200, 70, C_ACC, on_close, NULL);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
}

static void on_hist(lv_event_t *e)
{
    (void)e;
    show_history();
}

/* --- keypad --- */

typedef struct {
    int          value;
    int          who;
    bool         neg;
    mr_num_cb_t  cb;
    lv_obj_t    *disp;
    lv_obj_t    *who_b[MR_MAXP];
} pad_t;

static pad_t P;

static void pad_show(void)
{
    char b[24];
    snprintf(b, sizeof b, "%s%d", P.neg ? "-" : (P.cb ? "" : "+"), P.value);
    lv_label_set_text(P.disp, b);
    mr_game_t *g = mr_A->g;
    for (int i = 0; g && i < g->np && P.who >= 0; i++) {
        if (!P.who_b[i]) continue;
        lv_obj_set_style_border_width(P.who_b[i], i == P.who ? 4 : 0, 0);
        lv_obj_set_style_bg_opa(P.who_b[i], i == P.who ? 255 : 120, 0);
    }
}

static void pad_key(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    mr_snd_play(MR_S_TAP, 0.5f, 1.0f + 0.03f * (float)(k % 10));
    if (k >= 0 && k <= 9) {
        if (P.value < 100000) P.value = P.value * 10 + k;
    } else if (k == 10) {
        P.value /= 10;
    } else if (k == 11) {
        P.neg = !P.neg;
    } else if (k == 12) {
        int v = P.neg ? -P.value : P.value;
        mr_num_cb_t cb = P.cb;
        int who = P.who;
        close_sheet();
        if (cb) cb(v, who);
        else if (mr_A->g && v) mr_add(mr_A->g, who, v);
        return;
    } else if (k >= 100) {
        P.who = k - 100;
    }
    pad_show();
}

static void keypad_open(const char *title, int value, int who, mr_num_cb_t cb)
{
    mr_app_t *a = mr_A;
    memset(&P, 0, sizeof P);
    P.value = value < 0 ? -value : value;
    P.neg = value < 0;
    P.who = who;
    P.cb = cb;
    int w = a->land ? 620 : a->w - 40, h = a->land ? a->h - 40 : 980;
    lv_obj_t *card = sheet_open(w, h, title);
    w = lv_obj_get_width(card);
    h = lv_obj_get_height(card);
    int y = 84;
    mr_game_t *g = a->g;
    if (!cb && g) {
        lv_obj_t *r = lv_obj_create(card);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w - 40, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(r, 8, 0);
        lv_obj_set_style_pad_row(r, 8, 0);
        lv_obj_set_pos(r, 20, y);
        for (int i = 0; i < g->np; i++) {
            P.who_b[i] = mr_pill(r, g->p[i].name, LV_SIZE_CONTENT, 58, g->p[i].color, pad_key, (void *)(intptr_t)(100 + i));
            lv_obj_set_style_pad_hor(P.who_b[i], 20, 0);
            lv_obj_set_style_border_color(P.who_b[i], lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_update_layout(r);
        y += lv_obj_get_height(r) + 12;
    }
    P.disp = mr_label(card, aos_font_huge, 0xFFFFFF, "");
    lv_obj_set_width(P.disp, w - 60);
    lv_obj_set_style_text_align(P.disp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(P.disp, 30, y);
    y += 90;
    int kw = (w - 40 - 2 * 12) / 3, kh = (h - y - 20 - 4 * 12) / 5;
    if (kh > 100) kh = 100;
    static const int K[12] = { 7, 8, 9, 4, 5, 6, 1, 2, 3, 11, 0, 10 };
    for (int i = 0; i < 12; i++) {
        char t[16];
        if (K[i] <= 9) snprintf(t, sizeof t, "%d", K[i] % 10);
        else if (K[i] == 10) snprintf(t, sizeof t, LV_SYMBOL_BACKSPACE);
        else snprintf(t, sizeof t, "+/-");
        lv_obj_t *b = mr_pill(card, t, kw, kh, C_CARD2, pad_key, (void *)(intptr_t)K[i]);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_pos(b, 20 + (i % 3) * (kw + 12), y + (i / 3) * (kh + 12));
    }
    lv_obj_t *ok = mr_pill(card, cb ? _("Listo") : _("Anotar"), w - 40, kh, C_ACC, pad_key, (void *)(intptr_t)12);
    lv_obj_set_style_radius(ok, 18, 0);
    lv_obj_set_pos(ok, 20, y + 4 * (kh + 12));
    if (!a->land) lv_obj_set_height(card, y + 5 * (kh + 12) + 14);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    pad_show();
}

void mr_ui_number(const char *title, int value, int who, mr_num_cb_t cb)
{
    keypad_open(title, value, who, cb);
}

void mr_ui_keypad(int who)
{
    mr_game_t *g = mr_A->g;
    if (!g) return;
    if (who < 0 || who >= g->np) who = 0;
    keypad_open(_("Anotar puntos"), 0, who, NULL);
    /* counting down (darts): what is entered is taken off */
    if (g->type == MR_T_TIZA && g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && g->opt[MR_TIZA_SUB]) {
        P.neg = true;
        pad_show();
    }
}

/* --- menu --- */

static void on_menu_item(lv_event_t *e)
{
    mr_app_t *a = mr_A;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    close_sheet();
    if (!a->g) return;
    switch (k) {
    case 0: mr_ui_keypad(0); break;
    case 1:
        mr_reset(a->g, false);
        mr_snd_play(MR_S_UNDO, 0.6f, 0.7f);
        mr_toast(_("Cuenta en cero. Deshacer la vuelve atrás"));
        break;
    case 2: {
        /* players and rules: the form, over a copy */
        if (!a->draft) a->draft = mr_game_new(a->g->type);
        if (!a->draft) return;
        mr_hist_t *h = a->draft->hist;
        memcpy(a->draft, a->g, sizeof *a->draft);
        a->draft->hist = h;
        a->draft_edit = true;
        mr_save(a->g);
        /* the open game stays in a->g; the scene goes */
        mr_game_t *g = a->g;
        clear_root();
        a->g = g;
        show_setup();
        break;
    }
    case 3:
        a->sound = !a->sound;
        aos_hal_pref_set_i32("mr_sound", a->sound);
        mr_toast(a->sound ? _("Sonido prendido") : _("Sonido apagado"));
        break;
    default: break;
    }
}

static void on_menu(lv_event_t *e)
{
    (void)e;
    mr_app_t *a = mr_A;
    lv_obj_t *card = sheet_open(a->land ? 560 : a->w - 80, 520, NULL);
    int w = lv_obj_get_width(card);
    const char *items[4] = { _("Anotar puntos..."), _("Poner la cuenta en cero"), _("Jugadores y reglas"),
                             a->sound ? _("Apagar el sonido") : _("Prender el sonido") };
    const char *icons[4] = { LV_SYMBOL_PLUS, LV_SYMBOL_REFRESH, LV_SYMBOL_EDIT, a->sound ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = mr_pill(card, NULL, w - 40, 100, C_CARD2, on_menu_item, (void *)(intptr_t)i);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_set_pos(b, 20, 20 + i * 122);
        lv_obj_t *ic = mr_label(b, aos_font_body, C_ACC, icons[i]);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 28, 0);
        lv_obj_t *l = mr_label(b, aos_font_body, 0xFFFFFF, items[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 84, 0);
    }
}

/* --- the winner --- */

#define CONFETTI 46
typedef struct {
    lv_obj_t *o[CONFETTI];
    float     x[CONFETTI], y[CONFETTI], vx[CONFETTI], vy[CONFETTI], spin[CONFETTI], a[CONFETTI];
    bool      on;
} confetti_t;

static confetti_t CF;

static void confetti_frame(float dt)
{
    if (!CF.on || !mr_A->sheet) {
        CF.on = false;
        return;
    }
    int alive = 0;
    for (int i = 0; i < CONFETTI; i++) {
        if (!CF.o[i]) continue;
        CF.vy[i] += 380.0f * dt;
        CF.vx[i] *= 1.0f - 0.8f * dt;
        CF.x[i] += CF.vx[i] * dt;
        CF.y[i] += CF.vy[i] * dt;
        CF.a[i] += CF.spin[i] * dt;
        if (CF.y[i] > mr_A->h + 40) continue;
        alive++;
        lv_obj_set_pos(CF.o[i], (int)CF.x[i], (int)CF.y[i]);
        lv_obj_set_style_transform_rotation(CF.o[i], (int)CF.a[i], 0);
    }
    if (!alive) CF.on = false;
}

static void on_rematch(lv_event_t *e)
{
    (void)e;
    mr_app_t *a = mr_A;
    close_sheet();
    if (!a->g) return;
    mr_reset(a->g, true);
    mr_snd_play(MR_S_TAP, 0.8f, 0.9f);
}

void mr_ui_winner(int who)
{
    mr_app_t *a = mr_A;
    mr_game_t *g = a->g;
    if (!g || !a->scene_on) return;
    mr_snd_play(MR_S_WIN, 0.8f, 1.0f);
    lv_obj_t *card = sheet_open(a->land ? 640 : a->w - 80, 520, NULL);
    lv_obj_set_style_border_color(card, lv_color_hex(g->p[who].color), 0);
    lv_obj_set_style_border_width(card, 6, 0);
    lv_obj_t *cup = mr_label(card, aos_font_huge, g->p[who].color, LV_SYMBOL_OK);
    lv_obj_align(cup, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *t = mr_label(card, aos_font_large, 0xFFFFFF, "");
    lv_label_set_text_fmt(t, _("¡Ganó %s!"), g->p[who].name);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, lv_obj_get_width(card) - 40);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 120);
    char line[160];
    size_t k = 0;
    line[0] = 0;
    for (int i = 0; i < g->np && k < sizeof line - 30; i++)
        k += (size_t)snprintf(line + k, sizeof line - k, "%s%.12s: %d", i ? "   " : "", g->p[i].name, g->p[i].wins);
    lv_obj_t *s = mr_label(card, aos_font_small, C_DIM, "");
    lv_label_set_text_fmt(s, "%s  %s", _("Partidas ganadas:"), line);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s, lv_obj_get_width(card) - 40);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 250);
    int bw = (lv_obj_get_width(card) - 60) / 2;
    lv_obj_t *b1 = mr_pill(card, _("Seguir"), bw, 84, C_CARD2, on_close, NULL);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_LEFT, 20, -24);
    lv_obj_t *b2 = mr_pill(card, _("Revancha"), bw, 84, C_ACC, on_rematch, NULL);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_RIGHT, -20, -24);
    /* confetti over the dim, under nothing */
    uint32_t seed = mr_ms();
    memset(&CF, 0, sizeof CF);
    for (int i = 0; i < CONFETTI; i++) {
        uint32_t c = i % 3 == 0 ? g->p[who].color : mr_palette[(i * 7) % 10];
        lv_obj_t *o = box(a->sheet, 10 + (int)(mr_rnd(&seed) * 10), 18 + (int)(mr_rnd(&seed) * 10), c, 3);
        CF.o[i] = o;
        CF.x[i] = a->w * 0.5f + (mr_rnd(&seed) - 0.5f) * 120.0f;
        CF.y[i] = a->h * 0.42f;
        float ang = mr_rnd(&seed) * 6.2831853f;
        float sp = 300.0f + mr_rnd(&seed) * 700.0f;
        CF.vx[i] = cosf(ang) * sp;
        CF.vy[i] = -fabsf(sinf(ang)) * sp - 300.0f;
        CF.spin[i] = (mr_rnd(&seed) - 0.5f) * 3600.0f;
        lv_obj_set_pos(o, (int)CF.x[i], (int)CF.y[i]);
    }
    lv_obj_move_to_index(card, -1);
    CF.on = true;
}

/* --- the bar --- */

/* the undo arrow: the symbol fonts have none, so it is drawn */
static mr_img_t undo_icon;

static void undo_icon_make(void)
{
    if (undo_icon.px || !mr_img_new(&undo_icon, 40, 40)) return;
    float cx = 21, cy = 22, r = 11;
    float px = 0, py = 0;
    for (int i = 0; i <= 24; i++) {
        float a = (-200.0f + 230.0f * i / 24.0f) * 3.14159265f / 180.0f;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        if (i) mr_img_capsule(&undo_icon, px, py, x, y, 2.0f, 0xFFFFFF, 1.0f);
        px = x;
        py = y;
    }
    /* the head, at the left end, pointing down */
    float hx = cx - r * cosf(20.0f * 3.14159265f / 180.0f), hy = cy + r * sinf(20.0f * 3.14159265f / 180.0f);
    mr_img_capsule(&undo_icon, hx, hy + 1, hx - 7, hy - 6, 2.0f, 0xFFFFFF, 1.0f);
    mr_img_capsule(&undo_icon, hx, hy + 1, hx + 8, hy - 4, 2.0f, 0xFFFFFF, 1.0f);
}

void mr_ui_bar_refresh(void)
{
    mr_app_t *a = mr_A;
    if (!a || !a->bar_undo || !a->g) return;
    lv_obj_set_style_opa(a->bar_undo, a->g->nhist ? 255 : 110, 0);
}

static void bar_make(void)
{
    mr_app_t *a = mr_A;
    lv_obj_t *b = lv_obj_create(a->root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, a->w, BAR_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(b, 120, 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(b, 170, 0);
    lv_obj_set_style_bg_grad_opa(b, 60, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    a->bar = b;
    lv_obj_t *bk = mr_pill(b, LV_SYMBOL_LEFT, 72, 60, 0x33363B, on_bar_back, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 14, 0);
    a->bar_title = mr_label(b, aos_font_body, 0xFFFFFF, a->g->name);
    lv_label_set_long_mode(a->bar_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(a->bar_title, a->w - 104 - 3 * 84 - 20, lv_font_get_line_height(aos_font_body));
    lv_obj_align(a->bar_title, LV_ALIGN_LEFT_MID, 100, 0);
    lv_obj_t *m = mr_pill(b, LV_SYMBOL_BARS, 72, 60, 0x33363B, on_menu, NULL);
    lv_obj_align(m, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_t *h = mr_pill(b, LV_SYMBOL_LIST, 72, 60, 0x33363B, on_hist, NULL);
    lv_obj_align(h, LV_ALIGN_RIGHT_MID, -14 - 84, 0);
    undo_icon_make();
    a->bar_undo = mr_pill(b, NULL, 72, 60, 0x33363B, on_undo, NULL);
    lv_obj_t *ui = mr_img_obj(a->bar_undo, &undo_icon);
    lv_obj_center(ui);
    lv_obj_align(a->bar_undo, LV_ALIGN_RIGHT_MID, -14 - 168, 0);
    mr_ui_bar_refresh();
}

void mr_scene_rebuild(void)
{
    mr_app_t *a = mr_A;
    mr_game_t *g = a->g;
    if (!g) return;
    a->g = NULL;           /* clear_root must not save and free it */
    clear_root();
    a->g = g;
    a->screen = MR_SCR_GAME;
    mr_scene_t *s = &a->scene;
    memset(s, 0, sizeof *s);
    s->ops = OPS[g->type < MR_T_N ? g->type : 0];
    s->g = g;
    s->w = a->w;
    s->h = a->h;
    s->top = BAR_H;
    s->land = a->land;
    s->obj = lv_obj_create(a->root);
    mr_obj_plain(s->obj);
    lv_obj_set_size(s->obj, a->w, a->h);
    lv_obj_set_pos(s->obj, 0, 0);
    a->scene_on = true;
    uint32_t t0 = mr_ms();
    s->ops->build(s);
    if (s->ops->sync) s->ops->sync(s, MR_SYNC_LOAD, -1);
    aos_hal_log(MR_TAG, "%s built in %u ms (%dx%d)", mr_type_name(g->type), (unsigned)(mr_ms() - t0), a->w, a->h);
    bar_make();
    fingers_release();
    a->covered = false;
}

static void open_game(mr_game_t *g)
{
    mr_app_t *a = mr_A;
    if (a->g && a->g != g) {
        mr_save(a->g);
        mr_game_free(a->g);
    }
    a->g = g;
    aos_hal_pref_set_i32("mr_open", (int32_t)g->id);
    mr_scene_rebuild();
}

/* --- life ------------------------------------------------------------------------------- */

static void layout_from_root(void)
{
    mr_app_t *a = mr_A;
    lv_obj_update_layout(a->root);
    a->w = lv_obj_get_width(a->root);
    a->h = lv_obj_get_height(a->root);
    a->land = a->w > a->h;
}

static void show_current(void)
{
    mr_app_t *a = mr_A;
    switch (a->screen) {
    case MR_SCR_GAME:
        if (a->g) {
            mr_scene_rebuild();
            return;
        }
        break;
    case MR_SCR_NEW: show_new(); return;
    case MR_SCR_SETUP:
        if (a->draft) {
            show_setup();
            return;
        }
        break;
    default: break;
    }
    show_list();
}

static void *marcador_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)mr_alloc(sizeof *a);
    if (!a) return NULL;
    mr_A = a;
    a->root = root;
    for (int i = 0; i < MR_FINGERS; i++) f_btn[i] = -1;
    int32_t v = 1;
    aos_hal_pref_get_i32("mr_sound", &v);
    a->sound = v != 0;
    layout_from_root();
    uint32_t t0 = mr_ms();
    mr_snd_open();
    aos_hal_log(MR_TAG, "sounds in %u ms", (unsigned)(mr_ms() - t0));
    a->last_ms = a->tick_ms = mr_ms();
    a->timer = lv_timer_create(frame_cb, MR_FRAME_MS, a);

    /* back where it was: the game that was open when the app closed */
    int32_t open = 0;
    aos_hal_pref_get_i32("mr_open", &open);
    a->screen = MR_SCR_LIST;
#ifdef AOS_SIM_BUILTIN
    /* development (the simulator only; getenv() is NULL on the board):
     *   MR_DEMO=<kind>       a new game of that kind (mr_type_t)
     *   MR_NP=3              with that many players
     *   MR_SCORES=18,12      their scores
     *   MR_AUX=1:0,0:1       games:sets of each
     *   MR_OPT=0=2,1=10      options (index=value)
     *   MR_TARGET=30         the target
     *   MR_CLOCK=1421000     the stadium clock, ms into the period (MR_RUN=1 runs it) */
    const char *env = getenv("MR_DEMO");
    if (env && env[0]) {
        int type = atoi(env);
        mr_game_t *g = mr_game_new(type);
        if (g) {
            mr_game_defaults(g, type, getenv("MR_NP") ? atoi(getenv("MR_NP")) : 2);
            const char *e;
            if ((e = getenv("MR_TARGET"))) g->target = atoi(e);
            if ((e = getenv("MR_OPT"))) {
                while (*e) {
                    int k = atoi(e);
                    const char *q = strchr(e, '=');
                    if (!q) break;
                    if (k >= 0 && k < 8) g->opt[k] = atoi(q + 1);
                    const char *c = strchr(q, ',');
                    if (!c) break;
                    e = c + 1;
                }
            }
            if ((e = getenv("MR_SCORES"))) {
                for (int i = 0; i < g->np && *e; i++) {
                    g->p[i].score = atoi(e);
                    const char *c = strchr(e, ',');
                    if (!c) break;
                    e = c + 1;
                }
            }
            if ((e = getenv("MR_AUX"))) {
                for (int i = 0; i < g->np && *e; i++) {
                    g->p[i].aux[0] = (int16_t)atoi(e);
                    const char *q = strchr(e, ':');
                    if (q) g->p[i].aux[1] = (int16_t)atoi(q + 1);
                    const char *c = strchr(e, ',');
                    if (!c) break;
                    e = c + 1;
                }
            }
            if ((e = getenv("MR_CLOCK"))) g->clock_ms = atoi(e);
            if ((e = getenv("MR_PERIOD"))) g->period = (uint8_t)atoi(e);
            if ((e = getenv("MR_RUN"))) g->running = atoi(e) != 0;
            g->announced = true;    /* a demo does not open on the winner */
            g->id = mr_new_id();
            open_game(g);
            return a;
        }
    }
#endif
    if (open > 0) {
        mr_game_t *g = mr_game_new(0);
        if (g && mr_load(g, (uint32_t)open)) {
            open_game(g);
            return a;
        }
        mr_game_free(g);
    }
    show_list();
    return a;
}

static bool marcador_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)inst;
    if (!a) return false;
    if (a->screen == MR_SCR_SETUP) read_names();
    a->root = root;
    layout_from_root();
    show_current();
    return true;
}

static bool marcador_back(aos_app_t *self, void *inst)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)inst;
    if (!a) return false;
    if (a->kb) {
        kb_close();
        return true;
    }
    if (a->sheet) {
        close_sheet();
        return true;
    }
    switch (a->screen) {
    case MR_SCR_GAME: show_list(); return true;
    case MR_SCR_SETUP: on_setup_back(NULL); return true;
    case MR_SCR_NEW: show_list(); return true;
    default: return false;
    }
}

static void marcador_tick(aos_app_t *self, void *inst)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)inst;
    if (!a || !a->g || a->screen != MR_SCR_GAME) return;
    static uint32_t acc;
    acc += 200;
    if (acc >= 1000) {
        acc -= 1000;
        a->g->played_s++;
    }
}

static void marcador_hide(aos_app_t *self, void *inst)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)inst;
    if (a && a->g) mr_save(a->g);
}

static void marcador_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    mr_app_t *a = (mr_app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->timer = NULL;
    if (a->g) mr_save(a->g);
    int32_t open = a->screen == MR_SCR_GAME && a->g ? (int32_t)a->g->id : 0;
    aos_hal_pref_set_i32("mr_open", open);
    mr_game_t *g = a->g;
    a->g = NULL;
    /* the objects go first, then the memory they show */
    if (a->scene_on && a->scene.ops->free) a->scene.ops->free(&a->scene);
    a->scene_on = false;
    if (a->root) lv_obj_clean(a->root);
    mr_free(a->scene.st);
    thumbs_free();
    mr_img_free(&undo_icon);
    mr_game_free(g);
    mr_game_free(a->draft);
    mr_snd_close();
    memset(&CF, 0, sizeof CF);
    rbtn_forget();
    mr_A = NULL;
    mr_free(a);
}

/* The icon: a little scoreboard showing 2:1 over a square of matches
 * still being laid (four sticks and the fifth across). The blob has to fit
 * in AOS_ICON_OPS_MAX (256 bytes). */
static const uint8_t MARCADOR_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -10, 70, 44, 8, AIC_C_LIT(0x15171A), 255),
    AIC_BORDER(2, AIC_C_LIT(0x50555C), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, -16, 0, 22, 30, 4, AIC_C_LIT(0x2A1C05), 255),
    AIC_RECT(AIC_CENTER, 16, 0, 22, 30, 4, AIC_C_LIT(0x2A1C05), 255),
    /* a seven-segment 2 ... */
    AIC_RECT(AIC_CENTER, -16, -10, 11, 3, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_RECT(AIC_CENTER, -11, -5, 3, 10, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_RECT(AIC_CENTER, -16, 0, 11, 3, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_RECT(AIC_CENTER, -21, 5, 3, 10, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_RECT(AIC_CENTER, -16, 10, 11, 3, 1, AIC_C_LIT(0xFFB534), 255),
    /* ... and a 1 */
    AIC_RECT(AIC_CENTER, 18, 0, 3, 23, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_RECT(AIC_CENTER, 15, -9, 6, 3, 1, AIC_C_LIT(0xFFB534), 255),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, -24, 26, 4, 20, 2, AIC_C_LIT(0xF0D7A0), 255),
    AIC_RECT(AIC_CENTER, -12, 26, 4, 20, 2, AIC_C_LIT(0xF0D7A0), 255),
    AIC_RECT(AIC_CENTER, 0, 26, 4, 20, 2, AIC_C_LIT(0xF0D7A0), 255),
    AIC_RECT(AIC_CENTER, 12, 26, 4, 20, 2, AIC_C_LIT(0xF0D7A0), 255),
    AIC_RECT(AIC_CENTER, -6, 26, 44, 4, 2, AIC_C_LIT(0xF0D7A0), 255),
    AIC_ROT(-300),
    AIC_RECT(AIC_CENTER, -24, 16, 6, 5, AIC_CIRCLE, AIC_C_LIT(0xC8322A), 255),
    AIC_RECT(AIC_CENTER, -12, 16, 6, 5, AIC_CIRCLE, AIC_C_LIT(0xC8322A), 255),
    AIC_RECT(AIC_CENTER, 0, 16, 6, 5, AIC_CIRCLE, AIC_C_LIT(0xC8322A), 255),
    AIC_RECT(AIC_CENTER, 12, 16, 6, 5, AIC_CIRCLE, AIC_C_LIT(0xC8322A), 255),
    AIC_END
};

static bool marcador_init(aos_app_t *app)
{
    app->desc.id       = "aos.marcador";
    app->desc.name     = "Marcador";
    app->desc.icon     = LV_SYMBOL_LIST;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2F7D4A;
    app->desc.color_b  = 0x12361F;
    app->desc.order    = 96;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;
    aos_icon_set_ops(app, MARCADOR_ICON, sizeof MARCADOR_ICON);

    app->create  = marcador_create;
    app->destroy = marcador_destroy;
    app->back    = marcador_back;
    app->resize  = marcador_resize;
    app->tick    = marcador_tick;
    app->hide    = marcador_hide;
    return true;
}

AOS_APP_ENTRY(marcador_init);
