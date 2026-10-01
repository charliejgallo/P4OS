/*
 * GOLF - the app: life cycle, preferences, touch, the timer and the screens
 * that are LVGL panels (see gf_app.h for the map of the files)
 *
 * Every word on screen is an LVGL label wrapped in _(); the canvas only gets
 * pictures. Proper names (the holes, the course, the rivals) are not
 * translated.
 *
 * P4OS: every position comes from the screen's size (gfa_layout), and when
 * the screen turns the panels are built again for the new shape and the
 * canvas's background is made again (app_resize, gfp_refit); the round goes
 * on where it was.
 */
#include "gf_app.h"
#include "gf_audio.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS    33
#define PERF_MS     3000        /* how often the log says the frame rate      */

#define KEY_COINS   "gf_coins"
#define KEY_EQ      "gf_eq"
#define KEY_UNITS   "gf_units"
#define KEY_SFX     "gf_sfx"
#define KEY_DIFF    "gf_diff"
#define KEY_COURSE  "gf_course"
static const char *const KEY_OWN[CAT_N] = { "gf_own0", "gf_own1", "gf_own2", "gf_own3", "gf_own4", "gf_own5" };
static const char *const KEY_BEST[3] = { "gf_best0", "gf_best1", "gf_best2" };

static bool s_sfx = true;

void gf_sfx(int freq, int ms)
{
    if (s_sfx) gf_audio_tone(freq, ms);
}

void gf_sound(int id)
{
    if (s_sfx) gf_snd(id);
}

void gf_fmt_dist(const app_t *a, char *buf, int n, float m)
{
    if (a->metres) {
        if (m < 20.0f) snprintf(buf, (size_t)n, "%d.%d m", (int)m, (int)(m * 10) % 10);
        else snprintf(buf, (size_t)n, "%d m", (int)(m + 0.5f));
    } else {
        if (m < 18.0f) snprintf(buf, (size_t)n, "%d ft", (int)(m * 3.281f + 0.5f));
        else snprintf(buf, (size_t)n, "%d yd", (int)(m / GF_YD + 0.5f));
    }
}

/* --------------------------------------------------------------------------
 * The layout: where things go for the screen as it is now
 * -------------------------------------------------------------------------- */

void gfa_layout(app_t *a)
{
    gf_layout_t *L = &a->L;
    L->W = AOS_SCREEN_W;
    L->H = AOS_SCREEN_H;
    if (L->W * L->H > GF_MAXPX) {           /* never: the panel is 720x1280 */
        L->W = L->W > L->H ? 1280 : 720;
        L->H = L->W == 1280 ? 720 : 1280;
    }
    L->land = L->W > L->H;
    gf_w = L->W;
    gf_h = L->H;
    L->top_h = 72;
    L->bot_h = 112;
    L->map_top = L->top_h + 14;
    L->map_bot = L->H - L->bot_h - 12;
    /* the meter: on the right, the sweet spot low, 1.24 units of power tall */
    L->mtr_w = 36;
    L->mtr_x = L->W - 72;
    int mbot = L->land ? L->H - 44 : L->H - 210;
    float k = (float)(mbot - (L->top_h + 40)) / 1.24f;
    L->mtr_k = k > 430.0f ? 430.0f : k;
    L->mtr_zero = mbot - (int)(0.14f * L->mtr_k);
    L->wind_r = 26;
    L->wind_x = L->W - 48;
    L->wind_y = L->top_h + 48;
    /* the swing camera: the golfer (rendered at twice the watch's pixels)
     * stands with his feet 362 px under the principal point and the horizon
     * 187 px over it; upright there is room for sky, lying down for little */
    L->cam_cx = (float)(L->W / 2);
    L->cam_cy = L->land ? 318.0f : 700.0f;
    gf_cam_frame(L->cam_cx, L->cam_cy);
    /* the menu's buttons, beside the golfer */
    L->col_x = L->land ? 800 : 404;
    L->col_w = L->land ? 420 : L->W - L->col_x - 28;
}

/* --------------------------------------------------------------------------
 * Interface pieces
 * -------------------------------------------------------------------------- */

lv_obj_t *gfa_panel(lv_obj_t *parent, bool dim)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, AOS_SCREEN_W, AOS_SCREEN_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    if (dim) {
        lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_70, 0);
    }
    return p;
}

lv_obj_t *gfa_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

lv_obj_t *gfa_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                     uint32_t accent, const lv_font_t *font, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x12161C), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w - 12);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *all_panels(app_t *a, int i)
{
    lv_obj_t *const p[] = { a->p_menu, a->p_setup, a->p_pause, a->p_hole, a->p_round, a->p_settings, a->p_loading, a->p_shop, a->p_boot };
    return i < (int)(sizeof(p) / sizeof(p[0])) ? p[i] : NULL;
}

void gfa_show_panel(app_t *a, lv_obj_t *show)
{
    for (int i = 0; i < 9; i++) {
        lv_obj_t *p = all_panels(a, i);
        if (p && p != show) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
    if (show) {
        lv_obj_remove_flag(show, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(show);
    }
}

void gfa_hud_show(app_t *a, bool top, bool bottom)
{
    lv_obj_t *const tops[] = { a->hud_top, a->btn_pause };
    for (unsigned i = 0; i < 2; i++) {
        if (top) lv_obj_remove_flag(tops[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(tops[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *const bots[] = { a->hud_bot, a->lbl_lie };
    for (unsigned i = 0; i < 2; i++) {
        if (bottom) lv_obj_remove_flag(bots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void gfa_banner(app_t *a, const char *txt, uint32_t color, int ms)
{
    lv_label_set_text(a->banner, txt);
    lv_obj_set_style_text_color(a->banner, lv_color_hex(color), 0);
    lv_obj_remove_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->banner);
    lv_obj_set_user_data(a->banner, (void *)(intptr_t)ms);
}

void gfa_invalidate_all(app_t *a)
{
    lv_obj_invalidate(a->canvas);
}

/* --------------------------------------------------------------------------
 * The frame rate, in the log every few seconds while something moves: what
 * the swing, the flight and the menu really run at on the board.
 *   golf: swing 24.8 fps 720x1280, draw 3.1 ms, push 212 kpx/frame (panel ...)
 * "draw" is the game's own drawing on the canvas, "push" what it asks LVGL
 * to send (the dirty rectangles), "panel" what LVGL did send.
 * -------------------------------------------------------------------------- */

static const char *state_name(int st)
{
    switch (st) {
    case ST_MENU: return "menu";
    case ST_AIM: return "aim";
    case ST_SWING: return "swing";
    case ST_FLIGHT: return "flight";
    case ST_PUTT: return "putt";
    case ST_ROLL: return "roll";
    case ST_HOLE_END: return "card";
    case ST_SHOP: return "shop";
    default: return "other";
    }
}

void gfa_perf_frame(app_t *a, uint32_t draw_us, uint32_t px)
{
    static uint64_t s_fpx0;
    static uint32_t s_ffr0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!a->perf_t0) {
        a->perf_t0 = now;
        a->perf_frames = a->perf_draw_us = a->perf_px = 0;
        aos_hal_display_flush_count(&s_fpx0, &s_ffr0);
        return;
    }
    a->perf_frames++;
    a->perf_draw_us += draw_us;
    a->perf_px += px;
    uint32_t el = now - a->perf_t0;
    if (el < PERF_MS) return;
    unsigned fps10 = (unsigned)((uint64_t)a->perf_frames * 10000u / el);
    unsigned drw = a->perf_frames ? a->perf_draw_us / a->perf_frames : 0;
    unsigned kpx = a->perf_frames ? a->perf_px / a->perf_frames / 1000 : 0;
    uint64_t fpx = 0;
    uint32_t ffr = 0;
    unsigned flush_fps10 = 0, flush_kpx = 0;
    if (aos_hal_display_flush_count(&fpx, &ffr) && ffr > s_ffr0) {
        flush_fps10 = (unsigned)((uint64_t)(ffr - s_ffr0) * 10000u / el);
        flush_kpx = (unsigned)((fpx - s_fpx0) / (ffr - s_ffr0) / 1000);
    }
    s_fpx0 = fpx;
    s_ffr0 = ffr;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("golf", "%s %u.%u fps %dx%d, draw %u.%u ms, push %u kpx/frame (panel %u.%u fps, %u kpx) | "
                "game %u KB, psram free %u KB, internal %u KB",
                state_name(a->state), fps10 / 10, fps10 % 10, GF_W, GF_H, drw / 1000, drw / 100 % 10, kpx,
                flush_fps10 / 10, flush_fps10 % 10, flush_kpx, (unsigned)(gf_mem_used() / 1024),
                (unsigned)(hp / 1024), (unsigned)(hi / 1024));
    a->perf_t0 = now;
    a->perf_frames = a->perf_draw_us = a->perf_px = 0;
}

/* --------------------------------------------------------------------------
 * Preferences (the watch's keys: the progress carries over)
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    int32_t v;
    gf_wardrobe_default(&a->wr);
    if (aos_hal_pref_get_i32(KEY_COINS, &v)) a->wr.coins = v;
    for (int c = 0; c < CAT_N; c++) {
        if (aos_hal_pref_get_i32(KEY_OWN[c], &v)) a->wr.own[c] |= (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_EQ, &v)) {
        for (int c = 0; c < CAT_N; c++) {
            int it = (int)((uint32_t)v >> (c * 4)) & 15;
            if (it < gf_item_n(c) && gf_owns(&a->wr, c, it)) a->wr.eq[c] = (uint8_t)it;
        }
    }
    a->metres = aos_hal_pref_get_i32(KEY_UNITS, &v) && v == 1;
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) s_sfx = v != 0;
    a->sfx = s_sfx;
    if (aos_hal_pref_get_i32(KEY_DIFF, &v) && v >= 0 && v < DIFF_N) a->diff = v;
    else a->diff = DIFF_NORMAL;
    if (aos_hal_pref_get_i32(KEY_COURSE, &v) && v >= 0 && v < gf_course_n()) a->course = v;
    gf_course_select(a->course);
    for (int i = 0; i < 3; i++) {
        a->best[i] = aos_hal_pref_get_i32(KEY_BEST[i], &v) ? v : 999;
    }
}

void gfa_prefs_save(app_t *a)
{
    aos_hal_pref_set_i32(KEY_COINS, a->wr.coins);
    for (int c = 0; c < CAT_N; c++) aos_hal_pref_set_i32(KEY_OWN[c], (int32_t)a->wr.own[c]);
    uint32_t eq = 0;
    for (int c = 0; c < CAT_N; c++) eq |= (uint32_t)(a->wr.eq[c] & 15) << (c * 4);
    aos_hal_pref_set_i32(KEY_EQ, (int32_t)eq);
    aos_hal_pref_set_i32(KEY_UNITS, a->metres ? 1 : 0);
    aos_hal_pref_set_i32(KEY_SFX, s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_DIFF, a->diff);
    aos_hal_pref_set_i32(KEY_COURSE, a->course);
    for (int i = 0; i < 3; i++) aos_hal_pref_set_i32(KEY_BEST[i], a->best[i]);
}

/* --------------------------------------------------------------------------
 * States
 * -------------------------------------------------------------------------- */

static void menu_refresh(app_t *a)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)a->wr.coins);
    lv_label_set_text(a->lbl_coins, buf);
}

static void hole_end_show(app_t *a, bool effects);
static void round_end_fill(app_t *a);
static void round_end_apply(app_t *a);
static void setup_open(app_t *a, int mode);
static void settings_refresh(app_t *a);

/* The panels and the HUD a state shows. 'enter' is false when the screen
 * turned and the same state is shown again in the new layout: nothing
 * happens then but the showing (no sounds, no coins, no new jobs). */
static void state_show(app_t *a, int st, int old, bool enter)
{
    lv_obj_add_flag(a->btn_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->lbl_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->lbl_wind, LV_OBJ_FLAG_HIDDEN);
    switch (st) {
    case ST_MENU:
        gfa_hud_show(a, false, false);
        menu_refresh(a);
        if (enter && old != ST_MENU) gfp_menu_scene(a);
        /* the menu appears when its picture is there; until then, the
         * loading screen says what is happening */
        if (a->menu_ready) {
            gfa_show_panel(a, a->p_menu);
        } else {
            if (!a->booting && a->boot_pct > 60) a->boot_pct = 60;
            a->boot_target = 92;
            lv_label_set_text((lv_obj_t *)lv_obj_get_user_data(a->p_boot), gf_course()->name);
            gfa_show_panel(a, a->p_boot);
        }
        break;
    case ST_SETUP:
    case ST_SETTINGS:
        gfa_hud_show(a, false, false);
        gfa_show_panel(a, st == ST_SETUP ? a->p_setup : a->p_settings);
        break;
    case ST_LOADING:
        gfa_hud_show(a, false, false);
        gfa_show_panel(a, a->p_loading);
        break;
    case ST_AIM:
        gfa_show_panel(a, NULL);
        gfa_hud_show(a, true, true);
        lv_obj_remove_flag(a->lbl_wind, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(a->lbl_hit, _("Golpear"));
        break;
    case ST_PUTT:
        gfa_show_panel(a, NULL);
        gfa_hud_show(a, true, true);
        lv_label_set_text(a->lbl_hit, _("Pegar"));
        break;
    case ST_SWING:
        gfa_show_panel(a, NULL);
        gfa_hud_show(a, true, false);
        break;
    case ST_FLIGHT:
    case ST_ROLL:
    case ST_RESULT:
    case ST_REMOTE:
        gfa_show_panel(a, NULL);
        gfa_hud_show(a, true, false);
        break;
    case ST_HOLE_END:
        gfa_hud_show(a, false, false);
        hole_end_show(a, enter);
        gfa_show_panel(a, a->p_hole);
        break;
    case ST_ROUND_END:
        gfa_hud_show(a, false, false);
        if (enter) round_end_apply(a);
        round_end_fill(a);
        gfa_show_panel(a, a->p_round);
        break;
    case ST_SHOP:
        gfa_hud_show(a, false, false);
        gfa_show_panel(a, a->p_shop);
        if (enter) gfs_open(a);
        break;
    }
    lv_obj_move_foreground(a->banner);
}

void gfa_set_state(app_t *a, int st)
{
    int old = a->state;
    /* frames per second of the states that animate, in the log: what the
     * swing, the flight and the card really run at on the board */
    uint32_t el = (uint32_t)(aos_hal_uptime_ms() - a->st_t0);
    if (a->st_t0 && el > 1500 && (old == ST_SWING || old == ST_FLIGHT || old == ST_ROLL ||
                                  old == ST_HOLE_END || old == ST_MENU || old == ST_SHOP))
        aos_hal_log("golf", "state %d: %u frames in %u ms, %u.%u fps", old, (unsigned)a->st_frames,
                    (unsigned)el, (unsigned)(a->st_frames * 1000 / el),
                    (unsigned)(a->st_frames * 10000 / el % 10));
    a->st_t0 = aos_hal_uptime_ms();
    a->st_frames = 0;
    a->perf_t0 = 0;
    a->state = st;
    a->st_ms = 0;
    state_show(a, st, old, true);
    gf_audio_ambience(st >= ST_AIM && st <= ST_REMOTE);
}

/* --------------------------------------------------------------------------
 * Menu and setup
 * -------------------------------------------------------------------------- */

static const char *const MODE_NAME[MODE_N] = {
    N_("Juego rápido"), N_("Torneo"), N_("Práctica"), N_("Multijugador"), N_("Multijugador"),
};
static const char *const DIFF_NAME[DIFF_N] = { N_("Fácil"), N_("Normal"), N_("Profesional") };
static const char *const DIFF_DESC[DIFF_N] = {
    N_("Poco viento, medidor lento, ves dónde cae"),
    N_("Viento real, medidor ágil"),
    N_("Viento fuerte, medidor rápido, sin ayudas"),
};
static const uint32_t DIFF_COL[DIFF_N] = { 0x30D158, 0x0A84FF, 0xFF453A };

static void start_cb(lv_event_t *e);

typedef struct { app_t *a; int v; } cbv_t;
static cbv_t s_cbv[32];
static int s_ncbv;

static cbv_t *cbv(app_t *a, int v)
{
    if (s_ncbv >= 32) s_ncbv = 0;
    cbv_t *c = &s_cbv[s_ncbv++];
    c->a = a;
    c->v = v;
    return c;
}

static void diff_cb(lv_event_t *e)
{
    cbv_t *c = (cbv_t *)lv_event_get_user_data(e);
    c->a->diff = c->v;
    gfa_prefs_save(c->a);
    start_cb(e);
}

static void hole_pick_cb(lv_event_t *e)
{
    cbv_t *c = (cbv_t *)lv_event_get_user_data(e);
    c->a->practice_hole = c->v;
    lv_obj_t *box = c->a->setup_box;
    /* highlight the picked hole */
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); i++) {
        lv_obj_t *b = lv_obj_get_child(box, (int32_t)i);
        cbv_t *bc = (cbv_t *)lv_obj_get_user_data(b);
        if (bc && bc->v >= 100) {
            bool on = bc->v - 100 == c->v;
            lv_obj_set_style_bg_color(b, lv_color_hex(on ? 0x30D158 : 0x12161C), 0);
        }
    }
}

static void players_cb(lv_event_t *e)
{
    cbv_t *c = (cbv_t *)lv_event_get_user_data(e);
    app_t *a = c->a;
    if (c->v == 0) {
        a->mode = MODE_LINK;
        a->nplayers = 2;
        gfl_begin(a);
        return;
    }
    gfl_end(a);                 /* the radio went up to look for a partner */
    a->mode = MODE_LOCAL;
    a->nplayers = c->v;
    gfp_round_start(a);
}

static void start_cb(lv_event_t *e)
{
    cbv_t *c = (cbv_t *)lv_event_get_user_data(e);
    gfp_round_start(c->a);
}

static void course_step(app_t *a, int d)
{
    a->course = (a->course + d + gf_course_n()) % gf_course_n();
    gf_course_select(a->course);
    if (a->practice_hole >= gf_course()->nholes) a->practice_hole = 0;
    gfa_prefs_save(a);
    lv_label_set_text(a->lbl_menu_course, gf_course()->name);
    gf_sfx(900, 15);
    setup_open(a, a->mode == MODE_LINK ? MODE_LOCAL : a->mode);
}

static void course_prev_cb(lv_event_t *e) { course_step((app_t *)lv_event_get_user_data(e), -1); }
static void course_next_cb(lv_event_t *e) { course_step((app_t *)lv_event_get_user_data(e), 1); }

static void setup_open(app_t *a, int mode)
{
    a->mode = mode;
    a->nplayers = 1;
    lv_label_set_text(a->setup_title, _(MODE_NAME[mode]));
    lv_obj_clean(a->setup_box);
    s_ncbv = 0;
    lv_obj_update_layout(a->setup_box);
    int bw = lv_obj_get_width(a->setup_box);
    int y = 0;
    /* the course, on top of every setup screen */
    gfa_button(a->setup_box, LV_SYMBOL_LEFT, 0, 0, 60, 56, 0x8E8E93, &aos_montserrat_20, course_prev_cb, a);
    lv_obj_t *cn = gfa_label(a->setup_box, gf_course()->name, &aos_montserrat_24, 0xD8F0D8, 64, 14, bw - 128);
    lv_label_set_long_mode(cn, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(cn, 32);
    gfa_button(a->setup_box, LV_SYMBOL_RIGHT, bw - 60, 0, 60, 56, 0x8E8E93, &aos_montserrat_20, course_next_cb, a);
    y = 72;
    if (mode == MODE_LOCAL) {
        gfa_label(a->setup_box, _("En esta placa, pasándola"), &aos_montserrat_20, 0xA0A8B8, 0, y, bw);
        y += 38;
        for (int n = 2; n <= 4; n++) {
            char buf[32];
            snprintf(buf, sizeof buf, "%d %s", n, _("jugadores"));
            gfa_button(a->setup_box, buf, 0, y, bw, 64, 0x30D158, &aos_montserrat_24, players_cb, cbv(a, n));
            y += 76;
        }
        /* The other board: only when the link came up (on P4OS it rides
         * on ESP-NOW through the C6, which may not be there at all) */
        char nm[24];
        y += 8;
        if (gfl_available(a, nm, sizeof nm)) {
            char buf[48];
            snprintf(buf, sizeof buf, "%s %s", _("Contra"), nm);
            gfa_button(a->setup_box, buf, 0, y, bw, 68, 0xBF5AF2, &aos_montserrat_24, players_cb, cbv(a, 0));
        } else if (gfl_link_up()) {
            gfa_label(a->setup_box, _("Aparea otra placa en Enlace para jugar a distancia"), &aos_montserrat_16,
                      0x8A93A6, 0, y + 6, bw);
        }
    } else {
        if (mode == MODE_PRACTICE) {
            int n = gf_course()->nholes;
            int per = bw >= 480 ? 8 : 4;
            if (per > n) per = n;
            int cw = (bw + 10) / per - 10;
            for (int h = 0; h < n; h++) {
                char buf[8];
                snprintf(buf, sizeof buf, "%d", h + 1);
                lv_obj_t *b = gfa_button(a->setup_box, buf, (h % per) * (cw + 10), y + (h / per) * 66, cw, 56,
                                         0x30D158, &aos_montserrat_24, hole_pick_cb, cbv(a, h));
                lv_obj_set_user_data(b, cbv(a, 100 + h));
                if (h == a->practice_hole) lv_obj_set_style_bg_color(b, lv_color_hex(0x30D158), 0);
            }
            y += ((n + per - 1) / per) * 66 + 12;
        }
        for (int d = 0; d < DIFF_N; d++) {
            bool small = mode == MODE_PRACTICE;
            lv_obj_t *b = gfa_button(a->setup_box, "", 0, y, bw, small ? 64 : 92,
                                     DIFF_COL[d], &aos_montserrat_24, diff_cb, cbv(a, d));
            lv_obj_t *l = lv_obj_get_child(b, 0);
            lv_label_set_text(l, _(DIFF_NAME[d]));
            lv_obj_align(l, LV_ALIGN_TOP_MID, 0, small ? 16 : 12);
            if (!small) {
                lv_obj_t *s = gfa_label(b, _(DIFF_DESC[d]), &aos_montserrat_16, 0xA0A8B8, 8, 52, bw - 16);
                lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
                lv_obj_set_height(s, 22);
            }
            if (d == a->diff) lv_obj_set_style_border_width(b, 4, 0);
            y += small ? 76 : 104;
        }
    }
    gfa_set_state(a, ST_SETUP);
}

static void menu_quick_cb(lv_event_t *e) { setup_open((app_t *)lv_event_get_user_data(e), MODE_QUICK); }
static void menu_tour_cb(lv_event_t *e)  { setup_open((app_t *)lv_event_get_user_data(e), MODE_TOUR); }
static void menu_prac_cb(lv_event_t *e)  { setup_open((app_t *)lv_event_get_user_data(e), MODE_PRACTICE); }
static void menu_multi_cb(lv_event_t *e) { setup_open((app_t *)lv_event_get_user_data(e), MODE_LOCAL); }
static void menu_shop_cb(lv_event_t *e)  { gfa_set_state((app_t *)lv_event_get_user_data(e), ST_SHOP); }

static void settings_refresh(app_t *a)
{
    lv_label_set_text(lv_obj_get_child(a->chip_units, 0), a->metres ? _("Metros") : _("Yardas"));
    lv_label_set_text(lv_obj_get_child(a->chip_sfx, 0), s_sfx ? _("Sonido: sí") : _("Sonido: no"));
}

static void menu_settings_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    settings_refresh(a);
    gfa_set_state(a, ST_SETTINGS);
}

static void units_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->metres = !a->metres;
    gfa_prefs_save(a);
    settings_refresh(a);
}

static void sfx_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    s_sfx = !s_sfx;
    a->sfx = s_sfx;
    gf_audio_mute(!s_sfx);
    gfa_prefs_save(a);
    settings_refresh(a);
    gf_sfx(1200, 30);
}

static void to_menu_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->paused = false;
    if (a->mode == MODE_LINK) gfl_end(a);
    gfa_set_state(a, ST_MENU);
}

static void exit_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->leaving = true;
    a->want_exit = true;
}

/* --------------------------------------------------------------------------
 * Pause, hole card, round card
 * -------------------------------------------------------------------------- */

static void pause_show(app_t *a)
{
    if (a->state < ST_AIM || a->state > ST_REMOTE || a->state == ST_HOLE_END || a->state == ST_ROUND_END) return;
    a->paused = true;
    lv_obj_remove_flag(a->p_pause, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->p_pause);
}

static void pause_cb(lv_event_t *e)
{
    pause_show((app_t *)lv_event_get_user_data(e));
}

static void resume_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->paused = false;
    a->prev_ms = 0;
    lv_obj_add_flag(a->p_pause, LV_OBJ_FLAG_HIDDEN);
}

static void hit_cb(lv_event_t *e)     { gfp_hit_pressed((app_t *)lv_event_get_user_data(e)); }
static void prev_cb(lv_event_t *e)    { gfp_club_step((app_t *)lv_event_get_user_data(e), -1); }
static void next_cb(lv_event_t *e)    { gfp_club_step((app_t *)lv_event_get_user_data(e), 1); }
static void back3d_cb(lv_event_t *e)  { gfp_back((app_t *)lv_event_get_user_data(e)); }

static void fill_table(app_t *a, lv_obj_t *t)
{
    lv_obj_clean(t);
    const gf_game_t *g = &a->game;
    lv_obj_update_layout(t);            /* just built when the screen turned */
    int tw = lv_obj_get_width(t);
    char buf[64];
    int y = 0;
    int rows = g->nplayers + g->nrivals;
    /* sort: players and rivals by total */
    int order[GF_MAX_PLAYERS + GF_RIVALS], tot[GF_MAX_PLAYERS + GF_RIVALS];
    for (int i = 0; i < rows; i++) {
        order[i] = i;
        tot[i] = i < g->nplayers ? gf_game_total(g, i) : gf_game_rival_total(g, i - g->nplayers);
    }
    for (int i = 0; i < rows; i++)
        for (int j = i + 1; j < rows; j++)
            if (tot[order[j]] < tot[order[i]]) { int k = order[i]; order[i] = order[j]; order[j] = k; }
    int par = gf_game_par_so_far(g, g->hi);
    for (int r = 0; r < rows; r++) {
        int i = order[r];
        const char *nm;
        char pn[32];
        if (i < g->nplayers) {
            if (g->mode == MODE_LINK) nm = i == a->local_player ? _("Vos") : a->partner;
            else if (g->nplayers == 1) nm = _("Vos");
            else { snprintf(pn, sizeof pn, "%s %d", _("Jugador"), i + 1); nm = pn; }
        } else {
            nm = gf_rival_name(g->rival[i - g->nplayers].name);
        }
        int d = tot[i] - par;
        char rel[12];
        if (d == 0) snprintf(rel, sizeof rel, "E");
        else snprintf(rel, sizeof rel, "%+d", d);
        snprintf(buf, sizeof buf, "%d. %s", r + 1, nm);
        uint32_t col = i < g->nplayers ? 0xFFFFFF : 0xA0A8B8;
        lv_obj_t *l = gfa_label(t, buf, &aos_montserrat_20, col, 0, y, tw - 140);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(l, 26);
        snprintf(buf, sizeof buf, "%d  (%s)", tot[i], rel);
        l = gfa_label(t, buf, &aos_montserrat_20, d < 0 ? 0xFF6A5A : col, tw - 140, y, 140);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        y += 32;
    }
}

static void hole_end_show(app_t *a, bool effects)
{
    const gf_game_t *g = &a->game;
    int par = gf_course()->holes[gf_game_hole(g)].par;
    int me = g->mode == MODE_LINK ? a->local_player : 0;
    int s = g->pl[me].strokes[g->hi];
    char buf[64];
    lv_label_set_text(a->lbl_hole_t, g->pl[me].picked ? _("Recogida") : _(gf_score_name(s, par)));
    lv_obj_set_style_text_color(a->lbl_hole_t, lv_color_hex(s < par ? 0xFFD60A : 0xFFFFFF), 0);
    snprintf(buf, sizeof buf, "%s %d  ·  %d %s  ·  %s %d", _("Hoyo"), gf_game_hole(g) + 1, s, _("golpes"), _("Par"), par);
    lv_label_set_text(a->lbl_hole_s, buf);
    fill_table(a, a->hole_table);
    if (g->mode == MODE_PRACTICE) lv_label_set_text(a->lbl_hole_c, "");
    else {
        snprintf(buf, sizeof buf, "%s %d", _("Monedas"), gf_game_coins(g));
        lv_label_set_text(a->lbl_hole_c, buf);
    }
    if (!effects) return;
    if (s <= par - 1) {
        gf_sound(SND_GOOD);
        gf_sound(SND_APPLAUSE);
        gfp_react_begin(a, SEQ_CHEER);
    } else if (s >= par + 2 || g->pl[me].picked) {
        gf_sound(SND_BAD);
        gfp_react_begin(a, SEQ_SAD);
    } else {
        gfp_react_begin(a, SEQ_IDLE);
    }
}

/* the round's end: the coins and the record are taken once (apply), the
 * card is written from what was taken (fill), again if the screen turns */
static int  s_round_coins;
static bool s_round_record;

static void round_end_apply(app_t *a)
{
    const gf_game_t *g = &a->game;
    int me = g->mode == MODE_LINK ? a->local_player : 0;
    int to = gf_game_to_par(g, me);
    s_round_coins = gf_game_coins(g);
    a->wr.coins += s_round_coins;
    s_round_record = false;
    if (g->mode <= MODE_PRACTICE && g->mode != MODE_PRACTICE && to < a->best[g->mode]) {
        a->best[g->mode] = to;
        s_round_record = true;
    }
    gfa_prefs_save(a);
    gf_sound(SND_GOOD);
    if (g->mode == MODE_TOUR && gf_game_place(g) == 1) gf_sound(SND_APPLAUSE);
}

static void round_end_fill(app_t *a)
{
    const gf_game_t *g = &a->game;
    int me = g->mode == MODE_LINK ? a->local_player : 0;
    char buf[80];
    int tot = gf_game_total(g, me), to = gf_game_to_par(g, me);
    if (g->mode == MODE_TOUR) {
        int place = gf_game_place(g);
        snprintf(buf, sizeof buf, "%s %d", _("Terminaste"), place);
        lv_label_set_text(a->lbl_round_t, place == 1 ? _("¡Campeón!") : buf);
    } else if (g->mode == MODE_LINK) {
        int other = gf_game_total(g, 1 - me);
        lv_label_set_text(a->lbl_round_t, tot < other ? _("¡Ganaste!") : (tot == other ? _("Empate") : _("Perdiste")));
    } else {
        lv_label_set_text(a->lbl_round_t, s_round_record ? _("¡Nuevo récord!") : _("Fin de la vuelta"));
    }
    char rel[12];
    if (to == 0) snprintf(rel, sizeof rel, "E");
    else snprintf(rel, sizeof rel, "%+d", to);
    snprintf(buf, sizeof buf, "%d %s  (%s)", tot, _("golpes"), rel);
    lv_label_set_text(a->lbl_round_s, buf);
    fill_table(a, a->round_table);
    if (g->mode == MODE_PRACTICE) lv_label_set_text(a->lbl_round_c, "");
    else {
        snprintf(buf, sizeof buf, "+%d %s  ·  %s %d", s_round_coins, _("monedas"), _("Total"), (int)a->wr.coins);
        lv_label_set_text(a->lbl_round_c, buf);
    }
}

static void hole_next_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->art_busy) {
        /* the reactions are only for the card: give their memory back */
        gf_art_release(SEQ_CHEER);
        gf_art_release(SEQ_SAD);
    }
    if (a->game.mode == MODE_PRACTICE) {
        /* the same hole again */
        a->game.pl[0].strokes[0] = 0;
        gfp_hole_start(a);
        return;
    }
    if (gf_game_next_hole(&a->game)) gfp_hole_start(a);
    else gfa_set_state(a, ST_ROUND_END);
}

static void again_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->mode == MODE_LINK) {
        gfa_set_state(a, ST_MENU);
        return;
    }
    gfp_round_start(a);
}

/* --------------------------------------------------------------------------
 * Building the screens, for the layout of the moment
 * -------------------------------------------------------------------------- */

static void build_hud(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    int W = L->W, H = L->H;
    a->hud_top = lv_obj_create(root);
    lv_obj_remove_style_all(a->hud_top);
    lv_obj_set_size(a->hud_top, W, L->top_h);
    lv_obj_set_style_bg_color(a->hud_top, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->hud_top, LV_OPA_50, 0);
    lv_obj_remove_flag(a->hud_top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->hud_top, LV_OBJ_FLAG_SCROLLABLE);
    a->lbl_hole = gfa_label(a->hud_top, "", &aos_montserrat_24, 0xFFFFFF, 90, 6, W - 180);
    a->lbl_info = gfa_label(a->hud_top, "", &aos_montserrat_20, 0xD8E0EA, 90, 38, W - 180);
    lv_label_set_long_mode(a->lbl_hole, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(a->lbl_info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(a->lbl_hole, 30);
    lv_obj_set_height(a->lbl_info, 26);

    a->btn_pause = gfa_button(root, LV_SYMBOL_LIST, 12, 8, 64, 56, 0x8E8E93, &aos_montserrat_24, pause_cb, a);
    lv_obj_set_style_bg_opa(a->btn_pause, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(a->btn_pause, 0, 0);

    a->lbl_wind = gfa_label(root, "", &aos_montserrat_16, 0xFFFFFF, L->wind_x - 50, L->wind_y + L->wind_r + 4, 100);

    a->lbl_lie = gfa_label(root, "", &aos_montserrat_20, 0xFFFFFF, 20, H - L->bot_h - 46, 200);
    lv_obj_set_style_bg_color(a->lbl_lie, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->lbl_lie, LV_OPA_50, 0);
    lv_obj_set_style_radius(a->lbl_lie, 12, 0);
    lv_obj_set_style_pad_ver(a->lbl_lie, 4, 0);
    lv_obj_set_width(a->lbl_lie, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(a->lbl_lie, 12, 0);

    a->hud_bot = lv_obj_create(root);
    lv_obj_remove_style_all(a->hud_bot);
    lv_obj_set_size(a->hud_bot, W, L->bot_h);
    lv_obj_set_pos(a->hud_bot, 0, H - L->bot_h);
    lv_obj_set_style_bg_color(a->hud_bot, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->hud_bot, LV_OPA_50, 0);
    lv_obj_remove_flag(a->hud_bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->hud_bot, LV_OBJ_FLAG_CLICKABLE);
    int bh = 76, by = (L->bot_h - bh) / 2 - 4;
    a->btn_prev = gfa_button(a->hud_bot, LV_SYMBOL_LEFT, 18, by, 70, bh, 0x8E8E93, &aos_montserrat_24, prev_cb, a);
    a->lbl_club = gfa_label(a->hud_bot, "", &aos_montserrat_36, 0xFFFFFF, 92, by + 2, 170);
    a->lbl_carry = gfa_label(a->hud_bot, "", &aos_montserrat_20, 0xB8C2D0, 92, by + 48, 170);
    a->btn_next = gfa_button(a->hud_bot, LV_SYMBOL_RIGHT, 266, by, 70, bh, 0x8E8E93, &aos_montserrat_24, next_cb, a);
    int hw = L->land ? 280 : W - 360 - 18;
    a->btn_hit = gfa_button(a->hud_bot, "", W - hw - 18, by, hw, bh, 0x30D158, &aos_montserrat_28, hit_cb, a);
    lv_obj_set_style_bg_color(a->btn_hit, lv_color_hex(0x1E7A3A), 0);
    a->lbl_hit = lv_obj_get_child(a->btn_hit, 0);

    a->btn_back = gfa_button(root, LV_SYMBOL_LEFT, 14, L->top_h + 12, 64, 56, 0x8E8E93, &aos_montserrat_24, back3d_cb, a);
    if (L->land) a->lbl_hint = gfa_label(root, "", &aos_montserrat_20, 0xFFFFFF, 24, H - 58, 420);
    else a->lbl_hint = gfa_label(root, "", &aos_montserrat_20, 0xFFFFFF, 60, H - 110, W - 120);
    lv_label_set_text(a->lbl_hint, _("Tocá: carga, potencia, precisión"));
    lv_obj_set_style_bg_color(a->lbl_hint, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->lbl_hint, LV_OPA_50, 0);
    lv_obj_set_style_radius(a->lbl_hint, 12, 0);
    lv_obj_set_style_pad_ver(a->lbl_hint, 6, 0);

    a->banner = lv_label_create(root);
    lv_label_set_text(a->banner, "");
    lv_obj_set_style_text_font(a->banner, &aos_montserrat_36, 0);
    lv_obj_set_style_bg_color(a->banner, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->banner, LV_OPA_60, 0);
    lv_obj_set_style_radius(a->banner, 22, 0);
    lv_obj_set_style_pad_hor(a->banner, 22, 0);
    lv_obj_set_style_pad_ver(a->banner, 12, 0);
    lv_obj_set_style_text_align(a->banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(a->banner, W - 60, 0);
    lv_label_set_long_mode(a->banner, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(a->banner, LV_ALIGN_TOP_MID, 0, L->land ? 120 : 200);
    lv_obj_remove_flag(a->banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *coin_pill(lv_obj_t *parent, int x, int y, lv_obj_t **label)
{
    lv_obj_t *coin = lv_obj_create(parent);
    lv_obj_remove_style_all(coin);
    lv_obj_set_size(coin, 150, 42);
    lv_obj_set_pos(coin, x, y);
    lv_obj_set_style_bg_color(coin, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(coin, LV_OPA_50, 0);
    lv_obj_set_style_radius(coin, 21, 0);
    lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *dot = lv_obj_create(coin);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 26, 26);
    lv_obj_set_pos(dot, 10, 8);
    lv_obj_set_style_radius(dot, 13, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFD60A), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0xB8860B), 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    *label = gfa_label(coin, "0", &aos_montserrat_24, 0xFFD60A, 44, 7, 100);
    lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_LEFT, 0);
    return coin;
}

static void build_menu(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, false);
    a->p_menu = p;
    int cx = L->col_x, cw = L->col_w;
    int y;
    if (L->land) {
        gfa_label(p, "Golf", &aos_montserrat_64, 0x0A2A12, cx + 3, 33, cw);
        gfa_label(p, "Golf", &aos_montserrat_64, 0xFFFFFF, cx, 30, cw);
        a->lbl_menu_course = gfa_label(p, gf_course()->name, &aos_montserrat_24, 0xD8F0D8, cx, 112, cw);
        coin_pill(p, cx + (cw - 150) / 2, 152, &a->lbl_coins);
        y = 216;
    } else {
        /* upright: the title over the sky, the buttons beside the golfer */
        gfa_label(p, "Golf", &aos_montserrat_64, 0x0A2A12, 3, 63, L->W);
        gfa_label(p, "Golf", &aos_montserrat_64, 0xFFFFFF, 0, 60, L->W);
        a->lbl_menu_course = gfa_label(p, gf_course()->name, &aos_montserrat_24, 0xD8F0D8, 0, 142, L->W);
        coin_pill(p, L->W - 150 - 24, 24, &a->lbl_coins);
        y = 600;
    }
    int bh = 64, step = 78;
    gfa_button(p, _("Juego rápido"), cx, y, cw, bh, 0x30D158, &aos_montserrat_24, menu_quick_cb, a);
    gfa_button(p, _("Torneo"), cx, y + step, cw, bh, 0xFFD60A, &aos_montserrat_24, menu_tour_cb, a);
    gfa_button(p, _("Práctica"), cx, y + 2 * step, cw, bh, 0x64D2FF, &aos_montserrat_24, menu_prac_cb, a);
    gfa_button(p, _("Multijugador"), cx, y + 3 * step, cw, bh, 0xBF5AF2, &aos_montserrat_24, menu_multi_cb, a);
    int half = (cw - 12) / 2;
    gfa_button(p, _("Tienda"), cx, y + 4 * step + 6, half, bh, 0xFF9F0A, &aos_montserrat_20, menu_shop_cb, a);
    gfa_button(p, LV_SYMBOL_SETTINGS, cx + half + 12, y + 4 * step + 6, cw - half - 12, bh, 0x8E8E93,
               &aos_montserrat_24, menu_settings_cb, a);
}

static void setup_back_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    gfl_end(a);                 /* if the multiplayer screen started the radio */
    gfa_set_state(a, ST_MENU);
}

static int box_w(const app_t *a)
{
    int w = a->L.W - 60;
    return w > 560 ? 560 : w;
}

static void build_setup(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, true);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    a->p_setup = p;
    a->setup_title = gfa_label(p, "", &aos_montserrat_36, 0xFFFFFF, 90, L->land ? 26 : 200, L->W - 180);
    gfa_button(p, LV_SYMBOL_LEFT, 16, 20, 64, 56, 0x8E8E93, &aos_montserrat_24, setup_back_cb, a);
    int bw = box_w(a);
    a->setup_box = lv_obj_create(p);
    lv_obj_remove_style_all(a->setup_box);
    lv_obj_set_size(a->setup_box, bw, L->H - 120);
    lv_obj_set_pos(a->setup_box, (L->W - bw) / 2, L->land ? 96 : 300);
    lv_obj_remove_flag(a->setup_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->setup_box, LV_OBJ_FLAG_CLICKABLE);
}

static void build_settings(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, true);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    a->p_settings = p;
    gfa_label(p, _("Ajustes"), &aos_montserrat_36, 0xFFFFFF, 90, L->land ? 26 : 280, L->W - 180);
    gfa_button(p, LV_SYMBOL_LEFT, 16, 20, 64, 56, 0x8E8E93, &aos_montserrat_24, setup_back_cb, a);
    int bw = box_w(a) - 80, x = (L->W - bw) / 2, y = L->land ? 130 : 380;
    gfa_label(p, _("Distancias en"), &aos_montserrat_20, 0xA0A8B8, x, y, bw);
    a->chip_units = gfa_button(p, "", x, y + 36, bw, 68, 0x0A84FF, &aos_montserrat_24, units_cb, a);
    a->chip_sfx = gfa_button(p, "", x, y + 130, bw, 68, 0x30D158, &aos_montserrat_24, sfx_cb, a);
    gfa_label(p, _("Ganás monedas al jugar: más en torneo, en difícil y con birdies. Gastalas en la tienda."),
              &aos_montserrat_20, 0x8A93A6, x, y + 230, bw);
}

/* --------------------------------------------------------------------------
 * The loading screen: the first seconds, and whenever the menu's picture is
 * being prepared. A bar that always moves, so nobody thinks it hung.
 * -------------------------------------------------------------------------- */

static void splash_paint(app_t *a)
{
    /* a sky over mown grass, drawn in a few milliseconds: behind the first
     * screen instead of black */
    int hz = GF_H * 250 / 448;
    for (int y = 0; y < GF_H; y++) {
        uint16_t *row = a->fb + (size_t)y * GF_W;
        for (int x = 0; x < GF_W; x++) {
            int r, g, b;
            if (y < hz) {
                int t = y * 256 / hz;
                r = 52 + (186 - 52) * t / 256;
                g = 112 + (212 - 112) * t / 256;
                b = 200 + (232 - 200) * t / 256;
            } else {
                int t = (y - hz) * 256 / (GF_H - hz);
                int stripe = ((x + (y - hz) * 2) / 92) & 1 ? 10 : -6;
                r = 64 + 22 * t / 256 + stripe / 2;
                g = 140 + 34 * t / 256 + stripe;
                b = 52 + 12 * t / 256;
            }
            row[x] = gf_dither(r, g, b, x, y);
        }
    }
    gf_dirty_reset(&a->dprev);
    gf_dirty_reset(&a->dcur);
    a->bg = a->fb;
    lv_obj_invalidate(a->canvas);
}

void gfa_menu_ready(app_t *a)
{
    if (a->booting) return;           /* still loading the art */
    a->boot_pct = 100;
    lv_bar_set_value(a->boot_bar, 100, LV_ANIM_OFF);
    gfa_show_panel(a, a->p_menu);
    if (a->dev_card) {
        /* a birdie on the first hole, over the menu's picture */
        a->dev_card = false;
        gf_game_new(&a->game, MODE_QUICK, a->diff, 1, 1234, 0);
        a->game.holes[0] = 0;
        a->game.pl[0].strokes[0] = 3;
        a->game.pl[0].done = 1;
        gfa_set_state(a, ST_HOLE_END);
    }
}

static void boot_frame(app_t *a, int dt)
{
    (void)dt;
    /* towards the stage's target, and a creep even when it is reached, so
     * the bar never sits still */
    int want = a->boot_target;
    if (a->boot_pct < want) a->boot_pct += (want - a->boot_pct + 7) / 8;
    else if (a->boot_pct < want + 6 && a->boot_pct < 99 && (lv_tick_get() & 511) < 40) a->boot_pct++;
    lv_bar_set_value(a->boot_bar, a->boot_pct, LV_ANIM_OFF);
    static uint32_t last_dots = 99;
    uint32_t dots = (lv_tick_get() / 400) % 4;
    if (dots != last_dots) {
        last_dots = dots;
        char buf[64];
        snprintf(buf, sizeof buf, "%s%.*s", a->booting ? _("Cargando") : _("Preparando la cancha"), (int)dots, "...");
        lv_label_set_text(a->lbl_boot_st, buf);
    }
}

static void build_boot(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, false);
    a->p_boot = p;
    int ty = L->H * 110 / 448;
    gfa_label(p, "Golf", &aos_montserrat_64, 0x0A2A12, 3, ty + 3, L->W);
    gfa_label(p, "Golf", &aos_montserrat_64, 0xFFFFFF, 0, ty, L->W);
    lv_obj_t *cn = gfa_label(p, gf_course()->name, &aos_montserrat_28, 0xF0F8F0, 0, ty + 84, L->W);
    lv_obj_set_user_data(p, cn);
    int by = L->H * 350 / 448;
    a->lbl_boot_st = gfa_label(p, "", &aos_montserrat_20, 0xFFFFFF, 0, by - 42, L->W);
    lv_obj_t *bar = lv_bar_create(p);
    int bw = L->W * 220 / 368 > 460 ? 460 : L->W * 220 / 368;
    lv_obj_set_size(bar, bw, 14);
    lv_obj_set_pos(bar, (L->W - bw) / 2, by);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
    lv_obj_set_style_radius(bar, 7, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_value(bar, a->boot_pct, LV_ANIM_OFF);
    a->boot_bar = bar;
}

static void build_loading(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, true);
    a->p_loading = p;
    int y = L->H * 150 / 448;
    a->lbl_load_t = gfa_label(p, "", &aos_montserrat_28, 0xB8F0B8, 30, y, L->W - 60);
    a->lbl_load_s = gfa_label(p, "", &aos_montserrat_36, 0xFFFFFF, 30, y + 48, L->W - 60);
}

static void build_pause(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    lv_obj_t *p = gfa_panel(root, true);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    a->p_pause = p;
    int bw = 360, x = (L->W - bw) / 2, y = L->land ? 110 : 360;
    gfa_label(p, _("Pausa"), &aos_montserrat_48, 0xFFFFFF, 0, y, L->W);
    gfa_button(p, _("Seguir"), x, y + 96, bw, 76, 0x30D158, &aos_montserrat_32, resume_cb, a);
    gfa_button(p, _("Menú"), x, y + 190, bw, 66, 0x0A84FF, &aos_montserrat_24, to_menu_cb, a);
    gfa_button(p, _("Salir"), x, y + 272, bw, 66, 0xFF453A, &aos_montserrat_24, exit_cb, a);
}

static void build_cards(app_t *a, lv_obj_t *root)
{
    const gf_layout_t *L = &a->L;
    /* the hole card: a box, the golfer reacting beside or under it (the
     * canvas), the button in the corner */
    lv_obj_t *p = gfa_panel(root, false);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    a->p_hole = p;
    int bw = L->land ? 540 : L->W - 48, bh = 330;
    int bx = L->land ? L->W - bw - 30 : 24, by = 30;
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, bw, bh);
    lv_obj_set_pos(box, bx, by);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_70, 0);
    lv_obj_set_style_radius(box, 26, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    a->lbl_hole_t = gfa_label(box, "", &aos_montserrat_48, 0xFFFFFF, 10, 12, bw - 20);
    a->lbl_hole_s = gfa_label(box, "", &aos_montserrat_20, 0xB8C2D0, 10, 76, bw - 20);
    a->hole_table = lv_obj_create(box);
    lv_obj_remove_style_all(a->hole_table);
    lv_obj_set_size(a->hole_table, bw - 60, 136);
    lv_obj_set_pos(a->hole_table, 30, 116);
    lv_obj_remove_flag(a->hole_table, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->hole_table, LV_OBJ_FLAG_CLICKABLE);
    a->lbl_hole_c = gfa_label(box, "", &aos_montserrat_20, 0xFFD60A, 10, bh - 48, bw - 20);
    gfa_button(p, _("Continuar"), L->W - 250, L->H - (L->land ? 100 : 150), 220, 76, 0x30D158, &aos_montserrat_28,
               hole_next_cb, a);

    p = gfa_panel(root, true);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    a->p_round = p;
    int cw = box_w(a), cx = (L->W - cw) / 2, y = L->land ? 30 : 180;
    a->lbl_round_t = gfa_label(p, "", &aos_montserrat_48, 0xFFD60A, cx, y, cw);
    a->lbl_round_s = gfa_label(p, "", &aos_montserrat_28, 0xFFFFFF, cx, y + 66, cw);
    a->round_table = lv_obj_create(p);
    lv_obj_remove_style_all(a->round_table);
    lv_obj_set_size(a->round_table, cw - 40, 150);
    lv_obj_set_pos(a->round_table, cx + 20, y + 118);
    lv_obj_remove_flag(a->round_table, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->round_table, LV_OBJ_FLAG_CLICKABLE);
    a->lbl_round_c = gfa_label(p, "", &aos_montserrat_20, 0xFFD60A, cx, y + 280, cw);
    int bw2 = 360, x2 = (L->W - bw2) / 2;
    gfa_button(p, _("Otra vez"), x2, y + 330, bw2, 76, 0x30D158, &aos_montserrat_32, again_cb, a);
    gfa_button(p, _("Menú"), x2, y + 422, bw2, 66, 0x0A84FF, &aos_montserrat_24, to_menu_cb, a);
}

/* --------------------------------------------------------------------------
 * Touch, gestures, the frame
 * -------------------------------------------------------------------------- */

static void touch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing || a->paused) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    lv_event_code_t code = lv_event_get_code(e);
    int ev = code == LV_EVENT_PRESSED ? 0 : (code == LV_EVENT_PRESSING ? 1 : 2);
    int x = pt.x - co.x1, y = pt.y - co.y1;
    if (ev == 0) a->aim_pre = a->aim;
    if (a->pinching) return;            /* two fingers: gfp_pinch has the map */
    if (a->state == ST_SHOP) gfs_touch(a, x, y, ev);
    else gfp_touch(a, x, y, ev);
}

static void pinch_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (a->closing || a->paused) return;
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    gfp_pinch(a, ev, co.x1, co.y1);
}

static void handle_gesture(app_t *a, int dir)
{
    uint32_t now = lv_tick_get();
    if ((uint32_t)(now - a->last_gesture_ms) < 400) return;
    a->last_gesture_ms = now;
    if (dir != LV_DIR_RIGHT) return;
    if (a->state == ST_MENU) a->want_exit = true;
    else if (a->state == ST_SETUP || a->state == ST_SETTINGS || a->state == ST_SHOP) gfa_set_state(a, ST_MENU);
}

static void gesture_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (a->closing || !indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) handle_gesture(a, (int)dir);
}

static void frame(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->want_exit) {
        a->want_exit = false;
        aos_ui_back();
        return;
    }
    switch ((aos_touch_gesture_t)aos_ui_take_gesture()) {
    case AOS_TOUCH_GESTURE_RIGHT: handle_gesture(a, LV_DIR_RIGHT); break;
    default: break;
    }
    int dt = FRAME_MS;
    uint64_t now = aos_hal_uptime_ms();
    if (a->prev_ms && now > a->prev_ms) {
        uint32_t d = (uint32_t)(now - a->prev_ms);
        dt = d > 100 ? 100 : (int)d;
    }
    a->prev_ms = now;

    /* the banner's own clock */
    intptr_t bms = (intptr_t)lv_obj_get_user_data(a->banner);
    if (bms > 0) {
        bms -= dt;
        if (bms <= 0) {
            bms = 0;
            lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_user_data(a->banner, (void *)bms);
    }

    if (a->mode == MODE_LINK && a->link_on) gfl_tick(a);
    gf_audio_tick();
    if (a->booting || (a->state == ST_MENU && !a->menu_ready)) {
        boot_frame(a, dt);
        if (a->booting) {
            gfp_poll(a);
            return;
        }
    }
    if (a->paused) return;
    /* A panel, the switcher, a banner, the gesture home, the zoom to the
     * icon: the game waits under it (the flight stops in the air, the swing
     * and the menu's golfer hold still), so nothing is drawn for LVGL to
     * compose under them. A toast is let be. Everything on screen is
     * LVGL's (the canvas and the HUD over it), so when it goes LVGL has
     * already drawn the game where it was, and nothing is pushed again. The
     * worker's pictures still land. */
    uint32_t over = aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST;
    if (over != a->over) {
        if (!over || !a->over)
            aos_hal_log("golf", over ? "overlay 0x%02x: %s waits" : "overlay gone (was 0x%02x): %s goes on",
                        (unsigned)(over ? over : a->over), state_name(a->state));
        a->over = over;
        a->perf_t0 = 0;                 /* the fps log starts again after */
    }
    if (over) {
        a->prev_ms = 0;                 /* no jump in the game's clock after */
        gfp_poll(a);
        return;
    }
    a->st_ms += (uint32_t)dt;
    a->st_frames++;
    if (a->autoplay && a->st_ms > 2500 && a->state == ST_HOLE_END) {
        if (gf_game_next_hole(&a->game)) gfp_hole_start(a);
        else gfa_set_state(a, ST_ROUND_END);
        return;
    }
    if (a->autoplay && a->st_ms > 4000 && a->state == ST_ROUND_END) {
        gfp_round_start(a);
        return;
    }
    uint64_t t0 = aos_hal_uptime_us();
    switch (a->state) {
    case ST_MENU:
        gfp_menu_frame(a, dt);
        break;
    case ST_HOLE_END:
        gfp_poll(a);
        if (!a->fit_pending) gfp_react_frame(a);
        break;
    case ST_SHOP:
        gfp_poll(a);
        gfs_frame(a, dt);
        break;
    case ST_SETUP:
    case ST_SETTINGS:
    case ST_ROUND_END:
        gfp_poll(a);
        break;
    default:
        gfp_frame(a, dt);
        break;
    }
    /* what moves: the swing, the flight, the putt, the menu's golfer */
    if (a->state == ST_SWING || a->state == ST_FLIGHT || a->state == ST_ROLL || a->state == ST_AIM ||
        a->state == ST_PUTT || a->state == ST_MENU || a->state == ST_HOLE_END) {
        uint32_t px = 0;
        for (int i = 0; i < a->dprev.n; i++) {
            const gf_rect_t *r = &a->dprev.r[i];
            px += (uint32_t)((r->x1 - r->x0) * (r->y1 - r->y0));
        }
        gfa_perf_frame(a, (uint32_t)(aos_hal_uptime_us() - t0), px);
    }
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->leaving) return false;
    switch (a->state) {
    case ST_MENU:
        return false;
    case ST_SETUP:
    case ST_SETTINGS:
    case ST_SHOP:
    case ST_ROUND_END:
        gfa_set_state(a, ST_MENU);
        return true;
    case ST_LOADING:
        if (a->link_on && a->link_state == 1)      /* still in the lobby */ {
            gfl_end(a);
            gfa_set_state(a, ST_MENU);
        }
        return true;
    default:
        if (a->paused) {
            a->paused = false;
            lv_obj_add_flag(a->p_pause, LV_OBJ_FLAG_HIDDEN);
        } else if (!gfp_back(a)) {
            pause_show(a);
        }
        return true;
    }
}

static void golf_hide(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) pause_show((app_t *)inst);
}

static void free_all(app_t *a)
{
    gf_free(a->fb);
    gf_free(a->mapbuf);
    gf_free(a->v3dbuf);
    gf_free(a->v3dlow);
    gf_free(a->depth);
    gf_free(a->albedo);
    a->fb = a->mapbuf = a->v3dbuf = a->v3dlow = a->depth = a->albedo = NULL;
    a->zoombuf = NULL;
    gf_world_free(&a->world);
    gf_art_free();
}

/* The worker has read the pack and calibrated the clubs */
void gfa_booted(app_t *a)
{
    a->booting = false;
    a->boot_target = 92;
    a->state = -1;
    gfa_set_state(a, ST_MENU);
    aos_hal_log("golf", "booted | game %u KB (peak %u KB)", (unsigned)(gf_mem_used() / 1024),
                (unsigned)(gf_mem_peak() / 1024));
#ifdef AOS_SIM_BUILTIN
    /* Development switches (getenv() is NULL on the board):
     *   GF_MODE=0..3     straight into a round (quick, tour, practice, local 2p)
     *   GF_HOLE=1..8     the practice hole
     *   GF_DIFF=0..2
     *   GF_COINS=n       coins to spend in the shop
     *   GF_SCREEN=shop|settings|practice|boot|card
     */
    {
        const char *e;
        if ((e = getenv("GF_COINS")) && e[0]) a->wr.coins = atoi(e);
        if ((e = getenv("GF_AUTO")) && e[0]) a->autoplay = true;
        if ((e = getenv("GF_DIFF")) && e[0]) a->diff = atoi(e) % DIFF_N;
        if ((e = getenv("GF_COURSE")) && e[0]) {
            a->course = atoi(e) % gf_course_n();
            gf_course_select(a->course);
        }
        if ((e = getenv("GF_HOLE")) && e[0]) a->practice_hole = (atoi(e) - 1) % gf_course()->nholes;
        if ((e = getenv("GF_MODE")) && e[0]) {
            a->mode = atoi(e) % MODE_N;
            a->nplayers = a->mode == MODE_LOCAL || a->mode == MODE_LINK ? 2 : 1;
            if (a->mode == MODE_LINK) gfl_begin(a);
            else gfp_round_start(a);
        }
        if ((e = getenv("GF_SCREEN")) && e[0] == 'c') a->dev_card = true;
        if ((e = getenv("GF_SCREEN")) && e[0] == 'b') {
            /* GF_SCREEN=boot: the loading screen, held, to look at it */
            a->booting = true;
            a->boot_target = 60;
            splash_paint(a);
            gfa_show_panel(a, a->p_boot);
            return;
        }
        if ((e = getenv("GF_SCREEN")) && e[0]) {
            if (e[0] == 's' && e[1] == 'h') gfa_set_state(a, ST_SHOP);
            else if (e[0] == 's') { settings_refresh(a); gfa_set_state(a, ST_SETTINGS); }
            else if (e[0] == 'p') setup_open(a, MODE_PRACTICE);
        }
    }
#endif
}

/* Everything on the root, for the layout of the moment */
static void build_all(app_t *a, lv_obj_t *root)
{
    a->canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(a->canvas, a->fb, GF_W, GF_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, GF_W, GF_H);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_image_set_antialias(a->canvas, false);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_SCROLLABLE);

    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, AOS_SCREEN_W, AOS_SCREEN_H);
    lv_obj_set_pos(a->touch, 0, 0);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_RELEASED, a);
    aos_gesture_attach(a->touch, 0, pinch_cb, a);

    build_hud(a, root);
    build_menu(a, root);
    build_setup(a, root);
    build_settings(a, root);
    build_loading(a, root);
    build_boot(a, root);
    build_cards(a, root);
    gfs_build(a);
    build_pause(a, root);
    menu_refresh(a);
}

/* The screen turned. The panels are built again for the new shape and show
 * what they showed; the canvas gets a quick sky and grass at once, and the
 * worker makes the state's picture again (gfp_refit): the map for the new
 * view, the 3D view for the new frame, the menu's scene for the new side.
 * A round goes on where it was. */
static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->closing) return false;
    /* the words the panels hold, which only the flow that wrote them knows */
    lv_obj_t **keep[] = { &a->lbl_load_t, &a->lbl_load_s, &a->lbl_hint, &a->banner };
    static char txt[4][160];
    for (unsigned i = 0; i < 4; i++) snprintf(txt[i], sizeof txt[i], "%s", lv_label_get_text(*keep[i]));
    bool banner_on = !lv_obj_has_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
    bool hint_on = !lv_obj_has_flag(a->lbl_hint, LV_OBJ_FLAG_HIDDEN);
    bool back_on = !lv_obj_has_flag(a->btn_back, LV_OBJ_FLAG_HIDDEN);
    bool pause_on = !lv_obj_has_flag(a->p_pause, LV_OBJ_FLAG_HIDDEN);
    lv_color_t bcol = lv_obj_get_style_text_color(a->banner, 0);
    void *bms = lv_obj_get_user_data(a->banner);

    lv_obj_clean(root);
    gfa_layout(a);
    build_all(a, root);
    splash_paint(a);

    for (unsigned i = 0; i < 4; i++) lv_label_set_text(*keep[i], txt[i]);
    state_show(a, a->state, a->state, false);
    if (a->state == ST_SETUP) setup_open(a, a->mode == MODE_LINK ? MODE_LOCAL : a->mode);
    if (a->state == ST_SETTINGS) settings_refresh(a);
    if (a->state >= ST_AIM && a->state <= ST_RESULT) gfp_hud_refresh(a);
    if (hint_on) lv_obj_remove_flag(a->lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (back_on) lv_obj_remove_flag(a->btn_back, LV_OBJ_FLAG_HIDDEN);
    if (banner_on) {
        lv_obj_set_style_text_color(a->banner, bcol, 0);
        lv_obj_set_user_data(a->banner, bms);
        lv_obj_remove_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(a->banner);
    }
    if (pause_on) {
        lv_obj_remove_flag(a->p_pause, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(a->p_pause);
    }
    if (a->state == ST_SHOP) gfs_refit(a);
    gfp_refit(a);
    aos_hal_log("golf", "the screen turned: %dx%d | game %u KB", GF_W, GF_H, (unsigned)(gf_mem_used() / 1024));
    return true;
}

static void *golf_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("golf", "opening | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    gfa_layout(a);

    /* the screen-sized buffers hold either orientation */
    size_t scr = (size_t)GF_MAXPX * 2;
    size_t low = (size_t)(GF_MAXPX / 4) * 2;
    a->fb = (uint16_t *)gf_malloc(scr);
    a->mapbuf = (uint16_t *)gf_malloc(scr);
    a->v3dbuf = (uint16_t *)gf_malloc(scr);
    a->v3dlow = (uint16_t *)gf_malloc(low);
    a->depth = (uint16_t *)gf_malloc(low);
    /* the grid and the ground texture are sized for the biggest hole of
     * every course (the texture is half a metre per texel) */
    int cells = 0;
    for (int c = 0; c < gf_course_n(); c++) {
        const gf_course_t *co = gf_course_get(c);
        for (int h = 0; h < co->nholes; h++) {
            int gw, gh;
            gf_world_grid_size(&co->holes[h], &gw, &gh);
            if (gw * gh > cells) cells = gw * gh;
        }
    }
    a->albedo = (uint16_t *)gf_malloc((size_t)cells * 4 * 2);
    if (!a->fb || !a->mapbuf || !a->v3dbuf || !a->v3dlow || !a->depth || !a->albedo ||
        !gf_world_init(&a->world, cells)) {
        aos_hal_log("golf", "out of memory");
        free_all(a);
        lv_free(a);
        return NULL;
    }
    prefs_load(a);
    a->root = root;
    a->shown_player = -1;
    a->practice_hole = 0;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    memset(a->fb, 0, scr);
    build_all(a, root);

    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, a);

    if (s_sfx) gf_audio_open();
    gfp_worker_start(a);
    a->timer = lv_timer_create(frame, FRAME_MS, a);
    a->state = ST_LOADING;
    a->booting = true;
    gfa_hud_show(a, false, false);
    lv_obj_add_flag(a->btn_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->lbl_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->lbl_wind, LV_OBJ_FLAG_HIDDEN);
    a->boot_pct = 0;
    a->boot_target = 44;
    splash_paint(a);
    gfa_show_panel(a, a->p_boot);
    gfp_boot(a);

    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("golf", "ready %dx%d | game %u KB, internal %u B, psram %u B", GF_W, GF_H,
                (unsigned)(gf_mem_used() / 1024), (unsigned)hi, (unsigned)hp);
    return a;
}

static void golf_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    gfp_worker_stop();
    gf_audio_close();
    a->closing = true;
    if (a->link_on) gfl_end(a);
    gfl_close();
    if (a->root) lv_obj_clean(a->root);
    gfa_prefs_save(a);
    aos_hal_log("golf", "closing | game %u KB, peak %u KB", (unsigned)(gf_mem_used() / 1024),
                (unsigned)(gf_mem_peak() / 1024));
    free_all(a);
    lv_free(a);
}

/* The launcher icon: a flag on a green with the ball beside the cup. */
static const uint8_t GOLF_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  26, 78, 22, AIC_CIRCLE, AIC_C_LIT(0x3FAE48), 255),
    AIC_RECT(AIC_CENTER,  -6,  -6,  4, 60, 2,          AIC_C_TEXT,          255),
    AIC_RECT(AIC_CENTER,  10, -26, 30, 20, 3,          AIC_C_LIT(0xFF3B30), 255),
    AIC_RECT(AIC_CENTER,  -6,  24, 14,  6, AIC_CIRCLE, AIC_C_LIT(0x103018), 255),
    AIC_RECT(AIC_CENTER,  20,  20, 12, 12, AIC_CIRCLE, AIC_C_TEXT,          255),
    AIC_END
};

static bool golf_init(aos_app_t *app)
{
    app->desc.id       = "demo.golf";
    app->desc.name     = "Golf";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, GOLF_ICON, sizeof GOLF_ICON);
    app->desc.color_a  = 0x1E6B34;
    app->desc.color_b  = 0x0E3A5C;
    app->desc.order    = 158;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = golf_create;
    app->destroy = golf_destroy;
    app->hide    = golf_hide;
    app->back    = app_back;
    app->resize  = app_resize;
    return true;
}

AOS_APP_ENTRY(golf_init);
