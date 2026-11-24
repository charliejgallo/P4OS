/*
 * P4OS - VITRAL
 *
 * Pieces of coloured glass fall into a window of lead; a full row breaks.
 * The pieces are of three and of five squares (never four), the well is
 * twelve wide, and a full row of one colour is a "vitral": it scores three
 * times and takes the row under it with it. Clear glass goes with any
 * colour.
 *
 *   Maratón       levels every ten rows; it speeds up, and the awkward
 *                 pentominoes come in by level
 *   Contrarreloj  three minutes, the best score
 *   Rompecabezas  thirty windows to fill exactly with the pieces given
 *                 (vt_game.c makes them, and each one is checked to have a
 *                 solution); undo, a hint, stars by time
 *   De a dos      two wells on one board, side by side lying down or face
 *                 to face standing up; rows cleared send rubble to the other
 *
 * Drawn on the retro canvas (aos_retro.h) at 360x640 (or 640x360) shown x2:
 * every pixel of glass, lead and stone is drawn by code (vt_gfx.c), and
 * only the tiles that change are presented (vt_view.c). Words are LVGL
 * labels over it. 60 frames a second.
 *
 * The files are listed in vt_app.h.
 */
#include "vt_app.h"
#include "vt_snd.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A lancet window: lead, four panes and a round light at the top. */
static const uint8_t VITRAL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 2, 52, 62, 24, AIC_C_LIT(0x2A2C33), 255),
    AIC_RECT(AIC_CENTER, -11, -6, 20, 22, 4, AIC_C_LIT(0xD8283F), 255),
    AIC_RECT(AIC_CENTER, 11, -6, 20, 22, 4, AIC_C_LIT(0x2F66F0), 255),
    AIC_RECT(AIC_CENTER, -11, 19, 20, 22, 4, AIC_C_LIT(0xF2A81E), 255),
    AIC_RECT(AIC_CENTER, 11, 19, 20, 22, 4, AIC_C_LIT(0x22B862), 255),
    AIC_RECT(AIC_CENTER, 0, -22, 20, 12, AIC_CIRCLE, AIC_C_LIT(0xFFE08A), 255),
    AIC_END
};

#define KEY_HI_MAR   "vt_hi_mar"
#define KEY_HI_TIME  "vt_hi_time"
#define KEY_PZ_OPEN  "vt_pz_open"
#define KEY_PZ_S1    "vt_pz_s1"     /* stars of levels 1-15, two bits each */
#define KEY_PZ_S2    "vt_pz_s2"     /* and 16-30 */
#define KEY_OPTS     "vt_opts"      /* bit 0 keys, 1 no sound, 2 no ghost, 8.. start level */

vt_app_t *vt_A;

static const char *TAG = "vitral";

/* -------------------------------------------------------------------------- */
/* Preferences                                                                 */

static void prefs_load(vt_app_t *a)
{
    int32_t v = 0;
    a->sound = true;
    a->ghost = true;
    a->start_level = 1;
    a->pz_open = 1;
    if (aos_hal_pref_get_i32(KEY_HI_MAR, &v) && v > 0) a->hi_marathon = (uint32_t)v;
    if (aos_hal_pref_get_i32(KEY_HI_TIME, &v) && v > 0) a->hi_time = (uint32_t)v;
    if (aos_hal_pref_get_i32(KEY_PZ_OPEN, &v) && v > 0) a->pz_open = (uint8_t)(v > VT_PZ_LEVELS ? VT_PZ_LEVELS : v);
    for (int k = 0; k < 2; k++) {
        if (aos_hal_pref_get_i32(k ? KEY_PZ_S2 : KEY_PZ_S1, &v)) {
            for (int i = 0; i < 15; i++) a->pz_stars[k * 15 + i] = (uint8_t)(((uint32_t)v >> (2 * i)) & 3);
        }
    }
    if (aos_hal_pref_get_i32(KEY_OPTS, &v)) {
        a->use_keys = v & 1;
        a->sound = !(v & 2);
        a->ghost = !(v & 4);
        int lv = (v >> 8) & 0xFF;
        a->start_level = lv >= 1 && lv <= 15 ? lv : 1;
    }
}

void vt_prefs_save(vt_app_t *a)
{
    aos_hal_pref_set_i32(KEY_HI_MAR, (int32_t)a->hi_marathon);
    aos_hal_pref_set_i32(KEY_HI_TIME, (int32_t)a->hi_time);
    aos_hal_pref_set_i32(KEY_PZ_OPEN, a->pz_open);
    for (int k = 0; k < 2; k++) {
        uint32_t v = 0;
        for (int i = 0; i < 15; i++) v |= (uint32_t)(a->pz_stars[k * 15 + i] & 3) << (2 * i);
        aos_hal_pref_set_i32(k ? KEY_PZ_S2 : KEY_PZ_S1, (int32_t)v);
    }
    int32_t o = (a->use_keys ? 1 : 0) | (a->sound ? 0 : 2) | (a->ghost ? 0 : 4) | (a->start_level << 8);
    aos_hal_pref_set_i32(KEY_OPTS, o);
}

/* -------------------------------------------------------------------------- */
/* The buttons over the game                                                   */

static void on_pause(lv_event_t *e)
{
    (void)e;
    vt_set_paused(vt_A, true);
}

static void on_undo(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    if (a->paused || a->panel) return;
    vt_pz_undo(&a->p[0].g);
}

static void on_hint(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    if (a->paused || a->panel) return;
    a->hint_t = 150;
    vt_snd_play(VT_SND_TAP, 200);
}

static lv_obj_t *round_btn(vt_app_t *a, const char *sym, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(a->hud);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24212C), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3542), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, 220, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x8A7346), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &aos_sym_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xF2D58A), 0);
    if (txt) {
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, txt);
        lv_obj_set_style_text_font(t, aos_font_tiny, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0xF4EEDC), 0);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void hud_buttons(vt_app_t *a)
{
    a->btn_pause = a->btn_undo = a->btn_hint = NULL;
    if (a->screen != SCR_GAME) return;
    int W = lv_obj_get_width(a->root), H = lv_obj_get_height(a->root);
    a->btn_pause = round_btn(a, AOS_SYM_PAUSE, NULL, on_pause);
    lv_obj_set_size(a->btn_pause, 84, 72);
    if (a->nplayers == 2) {
        if (a->land) lv_obj_set_pos(a->btn_pause, W / 2 - 42, 8);
        else lv_obj_set_pos(a->btn_pause, W - 96, H / 2 - 36);
    } else {
        lv_obj_set_pos(a->btn_pause, W - 100, 14);
    }
    if (a->mode == VT_MODE_PUZZLE) {
        const vt_layout_t *L = &a->p[0].L;
        int bx = vt_sx(a, L->hx < 0 ? 4 : L->hx - 4), by = vt_sy(a, L->hy - 4);
        int bw = (L->nbox + 8) * a->r->scale;
        if (bw < 96) bw = 96;
        a->btn_undo = round_btn(a, AOS_SYM_RESTART, _("Deshacer"), on_undo);
        lv_obj_set_size(a->btn_undo, bw, 96);
        lv_obj_set_pos(a->btn_undo, bx, by);
        a->btn_hint = round_btn(a, AOS_SYM_LIGHTBULB_OUTLINE, _("Pista"), on_hint);
        lv_obj_set_size(a->btn_hint, bw, 96);
        lv_obj_set_pos(a->btn_hint, bx, by + 110);
    }
}

/* -------------------------------------------------------------------------- */
/* Games                                                                       */

static void build_screen(vt_app_t *a)
{
    vt_view_layout(a);
    if (a->screen == SCR_GAME) vt_view_game(a);
    else vt_view_title(a);
    vt_view_hud_build(a);
    hud_buttons(a);
}

void vt_relayout(vt_app_t *a)
{
    vt_panel_t k = a->panel_kind;
    build_screen(a);
    /* the panel goes back on top of what was rebuilt */
    if (a->panel) lv_obj_move_foreground(a->panel);
    (void)k;
}

void vt_start(vt_app_t *a, vt_mode_t mode, int level)
{
    vt_menu_close(a);
    a->mode = mode;
    a->screen = SCR_GAME;
    a->nplayers = mode == VT_MODE_VERSUS ? 2 : 1;
    a->paused = false;
    a->over = false;
    a->end_t = -1;
    a->new_record = false;
    a->hint_t = 0;
    a->seed = (uint32_t)aos_hal_uptime_us() * 2654435761u + 0x9E37u;
    for (int i = 0; i < 2; i++) {
        vt_player_t *p = &a->p[i];
        uint8_t *undo = p->g.undo;
        memset(&p->g, 0, sizeof p->g);
        p->g.undo = undo;
        p->lock_t = p->trail_t = p->ray_t = p->reject_t = p->level_t = 0;
    }
    if (mode == VT_MODE_PUZZLE) {
        a->pz_level = level;
        vt_init(&a->p[0].g, VT_MODE_PUZZLE, 1, 1);
        uint32_t t0 = (uint32_t)aos_hal_uptime_us();
        bool ok = vt_pz_build(&a->p[0].g, level);
        aos_hal_log(TAG, "puzzle %d %s in %u us (try %d, %d pieces)", level + 1, ok ? "built" : "FAILED",
                    (unsigned)((uint32_t)aos_hal_uptime_us() - t0), a->p[0].g.pz_try, a->p[0].g.pz_n);
    } else if (mode == VT_MODE_VERSUS) {
        /* the same pieces for both, in the same order */
        vt_init(&a->p[0].g, mode, a->seed, 3);
        vt_init(&a->p[1].g, mode, a->seed, 3);
        a->p[1].g.gseed = a->p[0].g.gseed;
    } else {
        vt_init(&a->p[0].g, mode, a->seed, mode == VT_MODE_TIME ? 1 : level);
    }
    for (int i = 0; i < a->nplayers; i++) vt_events_clear(&a->p[i].g);
    vt_input_reset(a);
    build_screen(a);
    vt_snd_play(VT_SND_LEVEL, 200);
    aos_hal_log(TAG, "start | mode %d level %d", mode, level);
}

void vt_quit_to_title(vt_app_t *a)
{
    vt_menu_close(a);
    a->screen = SCR_TITLE;
    a->nplayers = 1;
    a->paused = false;
    a->over = false;
    build_screen(a);
    vt_menu_title(a);
}

void vt_set_paused(vt_app_t *a, bool p)
{
    if (a->screen != SCR_GAME) return;
    if (p) {
        if (a->over || a->panel) return;
        a->paused = true;
        vt_menu_pause(a);
    } else {
        a->paused = false;
        vt_menu_close(a);
        vt_input_reset(a);
    }
}

/* the end of a game: records, then the panel a moment later */
void vt_game_ended(vt_app_t *a)
{
    a->over = true;
    a->end_t = 1300;
    vt_game_t *g = &a->p[0].g;
    if (a->mode == VT_MODE_MARATHON && g->score > a->hi_marathon) {
        a->hi_marathon = g->score;
        a->new_record = true;
    }
    if (a->mode == VT_MODE_TIME && g->score > a->hi_time) {
        a->hi_time = g->score;
        a->new_record = true;
    }
    if (a->mode == VT_MODE_PUZZLE) {
        if (g->state == VT_WON) {
            uint32_t par = vt_pz_par(g);
            a->stars_now = g->time_ms <= par ? 3 : g->time_ms <= par * 2 ? 2 : 1;
            if (a->stars_now > a->pz_stars[a->pz_level]) a->pz_stars[a->pz_level] = (uint8_t)a->stars_now;
            if (a->pz_level + 2 > a->pz_open) a->pz_open = (uint8_t)(a->pz_level + 2 > VT_PZ_LEVELS ? VT_PZ_LEVELS : a->pz_level + 2);
            a->end_t = 1800;
        } else {
            a->end_t = 500;
        }
    }
    if (a->mode == VT_MODE_VERSUS) {
        a->winner = a->p[0].g.state == VT_OVER ? 1 : 0;
        if (a->wins[a->winner] < 99) a->wins[a->winner]++;
    }
    vt_prefs_save(a);
}

/* -------------------------------------------------------------------------- */
/* The frame                                                                   */

static void step(void *user)
{
    vt_app_t *a = (vt_app_t *)user;
    if (a->hidden) return;
    a->step_acc += 1000;
    uint32_t ms = a->step_acc / VT_FPS;
    a->step_acc %= VT_FPS;
    vt_snd_pump();
    vt_menu_frame(a);
    if (a->screen != SCR_GAME) {
        a->now += ms;
        return;
    }
    if (!a->paused) a->now += ms;
    if (a->paused) return;

    if (!a->over) vt_input_frame(a);
    if (a->paused) return;
    for (int i = 0; i < a->nplayers; i++) {
        vt_game_t *g = &a->p[i].g;
        vt_tick(g, a->over && g->state != VT_CLEARING ? 0 : ms);
        vt_view_events(a, i);
        vt_events_clear(g);
    }
    if (a->nplayers == 2) {
        for (int i = 0; i < 2; i++) {
            vt_game_t *g = &a->p[i].g, *o = &a->p[1 - i].g;
            if (g->garbage_out) {
                vt_add_garbage(o, g->garbage_out);
                g->garbage_out = 0;
            }
        }
    }
    if (!a->over) {
        bool end = false;
        for (int i = 0; i < a->nplayers; i++) {
            vt_state_t s = a->p[i].g.state;
            if (s == VT_OVER || s == VT_WON || s == VT_STUCK) end = true;
        }
        if (end) vt_game_ended(a);
    } else if (a->end_t >= 0) {
        a->end_t -= (int32_t)ms;
        if (a->end_t < 0 && !a->panel) vt_menu_over(a);
    }
}

static void draw(void *user)
{
    vt_app_t *a = (vt_app_t *)user;
    if (a->hidden) return;
    vt_view_frame(a);
    vt_view_hud(a);
    a->fps_frames++;
    uint32_t now = lv_tick_get();
    if (now - a->fps_t0 >= 5000) {
        aos_retro_stats_t st;
        aos_retro_stats(&st);
        aos_hal_log(TAG, "%u.%u fps | draw %u us avg, %u max | refresh %u us | parts %d",
                    (unsigned)(st.fps10 / 10), (unsigned)(st.fps10 % 10),
                    (unsigned)(a->draw_us / (a->fps_frames ? a->fps_frames : 1)), (unsigned)a->draw_max,
                    (unsigned)st.refresh_us_avg, a->nparts);
        a->fps_frames = 0;
        a->draw_us = a->draw_max = 0;
        a->fps_t0 = now;
    }
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */

static bool begin_canvas(vt_app_t *a, lv_obj_t *root)
{
    lv_obj_update_layout(root);
    int w = lv_obj_get_width(root), h = lv_obj_get_height(root);
    a->land = w > h;
    a->cw = w / 2;
    a->ch = h / 2;
    if (a->cw > VT_TILES_X * VT_TILE) a->cw = VT_TILES_X * VT_TILE;
    if (a->ch > VT_TILES_Y * VT_TILE) a->ch = VT_TILES_Y * VT_TILE;
    a->r = aos_retro_begin(root, a->cw, a->ch, 2, 0);
    if (!a->r) return false;
    aos_retro_set_border(0x0A090E);
    a->root = root;
    return true;
}

static bool key_cb(uint32_t key, uint8_t mods)
{
    return vt_input_key(key, mods);
}

static void *vt_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)malloc(sizeof *a);
    if (!a) return NULL;
    memset(a, 0, sizeof *a);
    vt_A = a;
    a->bg = (uint16_t *)malloc((size_t)VT_TILES_X * VT_TILE * 360 * 2);
    a->p[0].g.undo = (uint8_t *)malloc(vt_pz_undo_size() * VT_PZ_MAX_SEQ);
    if (!a->bg || !a->p[0].g.undo || !begin_canvas(a, root)) {
        aos_hal_log(TAG, "out of memory");
        aos_retro_end();
        free(a->bg);
        free(a->p[0].g.undo);
        free(a);
        vt_A = NULL;
        return NULL;
    }
    prefs_load(a);
    a->end_t = -1;
    a->nplayers = 1;
    a->screen = SCR_TITLE;
    vt_snd_enable(a->sound);
    vt_snd_open();
    aos_ui_hwkbd_handler(key_cb);
    build_screen(a);
    vt_menu_title(a);

#ifdef AOS_SIM_BUILTIN
    /* Shortcuts for the simulator (no environment on the board):
     *   VT_MODE=0..3     starts that mode at once (3 = de a dos)
     *   VT_LEVEL=n       the level (the puzzle's, from 0)
     *   VT_KEYS=1        the on-screen keys
     *   VT_DEMO=n        plays n pieces by itself, to look at
     *   VT_SND_DUMP=dir  writes the sounds as WAVs */
    if (getenv("VT_SND_DUMP")) vt_snd_dump(getenv("VT_SND_DUMP"));
    if (getenv("VT_KEYS")) a->use_keys = atoi(getenv("VT_KEYS")) != 0;
    if (getenv("VT_MODE")) {
        int lv = getenv("VT_LEVEL") ? atoi(getenv("VT_LEVEL")) : (atoi(getenv("VT_MODE")) == 2 ? 0 : 1);
        vt_start(a, (vt_mode_t)atoi(getenv("VT_MODE")), lv);
        if (getenv("VT_DEMO")) {
            extern void vt_demo(vt_app_t *, int);
            vt_demo(a, atoi(getenv("VT_DEMO")));
        }
        if (getenv("VT_PZ_PLACE") && a->mode == VT_MODE_PUZZLE) {
            /* the first n pieces where the generator put them */
            vt_game_t *g = &a->p[0].g;
            int n = atoi(getenv("VT_PZ_PLACE"));
            for (int i = 0; i < n && g->state == VT_PLAY && g->pz_i < g->pz_n; i++) {
                g->cur.rot = g->pz_sol[g->pz_i].rot;
                g->cur.x = g->pz_sol[g->pz_i].x;
                vt_hard(g);
                vt_events_clear(g);
            }
            a->p[0].board_all = true;
        }
        if (getenv("VT_SCENE")) {
            extern void vt_scene_set(vt_app_t *, int);
            vt_scene_set(a, atoi(getenv("VT_SCENE")));
        }
    }
#endif

    a->fps_t0 = lv_tick_get();
    aos_retro_run(VT_FPS, step, draw, a);
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log(TAG, "ready | canvas %dx%d x%d | internal %u, psram %u", a->cw, a->ch, a->r->scale, (unsigned)hi,
                (unsigned)hp);
    return a;
}

static bool vt_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)inst;
    if (!a) return false;
    vt_panel_t kind = a->panel_kind;
    aos_pad_menu_clear(&a->menu);
    if (a->panel) lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PANEL_NONE;
    if (a->hud) lv_obj_delete(a->hud);
    a->hud = NULL;
    aos_retro_end();
    a->r = NULL;
    if (!begin_canvas(a, root)) return false;
    build_screen(a);
    /* the panel that was up comes back for the new shape */
    switch (kind) {
    case PANEL_TITLE: vt_menu_title(a); break;
    case PANEL_LEVELS: { extern void vt_menu_levels(vt_app_t *); vt_menu_levels(a); break; }
    case PANEL_OPTIONS: { extern void vt_menu_options(vt_app_t *); vt_menu_options(a); break; }
    case PANEL_HELP: { extern void vt_menu_help(vt_app_t *); vt_menu_help(a); break; }
    case PANEL_PAUSE: vt_menu_pause(a); break;
    case PANEL_OVER: case PANEL_WON: case PANEL_STUCK: case PANEL_VS_OVER: vt_menu_over(a); break;
    default:
        /* a game in the middle of a turn of the screen waits under the pause */
        if (a->screen == SCR_GAME && !a->over) {
            a->paused = false;
            vt_set_paused(a, true);
        }
        break;
    }
    aos_retro_run(VT_FPS, step, draw, a);
    aos_hal_log(TAG, "turned | canvas %dx%d", a->cw, a->ch);
    return true;
}

static void vt_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)inst;
    if (!a) return;
    vt_prefs_save(a);
    aos_ui_hwkbd_handler(NULL);
    aos_retro_end();
    vt_snd_free();
    free(a->bg);
    free(a->p[0].g.undo);
    free(a);
    vt_A = NULL;
}

static void vt_hide(aos_app_t *self, void *inst)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)inst;
    if (!a) return;
    if (a->screen == SCR_GAME && !a->paused && !a->over && !a->panel) vt_set_paused(a, true);
    a->hidden = true;
    vt_snd_close();
}

static void vt_show(aos_app_t *self, void *inst)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)inst;
    if (!a) return;
    a->hidden = false;
    vt_snd_open();
    aos_ui_hwkbd_handler(key_cb);
    aos_pad_reset(&a->mpad, lv_tick_get());
}

static bool vt_back(aos_app_t *self, void *inst)
{
    (void)self;
    vt_app_t *a = (vt_app_t *)inst;
    if (!a) return false;
    if (a->screen == SCR_GAME && !a->panel && !a->over) {
        vt_set_paused(a, true);
        return true;
    }
    if (a->panel_kind == PANEL_PAUSE) return false;     /* the pause has its own way out */
    return vt_menu_back(a);
}

static bool vt_init_app(aos_app_t *app)
{
    app->desc.id = "demo.vitral";
    app->desc.name = "Vitral";
    app->desc.icon = "V";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0xC8203C;
    app->desc.color_b = 0x2A5CE6;
    app->desc.order = 160;
    /* LONG_DRAG: a drag across the well is longer than LVGL's gesture limit
     * and must not lose its release (a tap turns the piece); NO_SWIPE: a
     * drag from the edge is play. No orientation flag: both ways, resize()
     * keeps the game. */
    app->desc.flags = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_NO_SWIPE |
                      AOS_APP_FLAG_LONG_DRAG;
    aos_icon_set_ops(app, VITRAL_ICON, sizeof VITRAL_ICON);

    app->create = vt_create;
    app->destroy = vt_destroy;
    app->show = vt_show;
    app->hide = vt_hide;
    app->back = vt_back;
    app->resize = vt_resize;
    return true;
}

AOS_APP_ENTRY(vt_init_app);

/* -------------------------------------------------------------------------- */

#ifdef AOS_SIM_BUILTIN
/* VT_SCENE=1: a stack of glass with a gap down column 9 whose bottom row is
 * all sapphire, and a sapphire bar of three right over the gap: one drop
 * and two rows break, one of them a vitral. 2: the same without the bar
 * (a busy board for the pictures). */
void vt_scene_set(vt_app_t *a, int kind)
{
    for (int pi = 0; pi < a->nplayers; pi++) {
        vt_game_t *g = &a->p[pi].g;
        static const uint8_t TOPS[2][VT_COLS_MAX] = {
            { 17, 16, 15, 16, 17, 18, 17, 16, 17, VT_ROWS, 18, 17 },
            { 19, 18, 17, 17, 18, 19, 18, 17, 18, VT_ROWS, 18, 19 },
        };
        memset(g->col, 0, sizeof g->col);
        memset(g->pid, 0, sizeof g->pid);
        if (!vt_scene(g, TOPS[pi], 11u + (uint32_t)kind + (uint32_t)pi * 7u)) continue;
        if (kind == 1 && pi == 0) {
            /* the pieces touching the bottom row turn sapphire */
            uint16_t ids[VT_COLS_MAX];
            for (int x = 0; x < g->cols; x++) ids[x] = g->pid[VT_ROWS - 1][x];
            for (int y = 0; y < VT_ROWS; y++)
                for (int x = 0; x < g->cols; x++)
                    for (int k = 0; k < g->cols; k++)
                        if (ids[k] && g->pid[y][x] == ids[k]) g->col[y][x] = VT_SAPPHIRE;
            g->cur.type = PC_I3;
            g->cur.rot = 1;
            g->cur.color = VT_SAPPHIRE;
            g->cur.x = 9 - 2;
            g->cur.y = 8;
        }
    }
    for (int i = 0; i < a->nplayers; i++) a->p[i].board_all = true;
}

/* plays n pieces with a simple bot, without drawing in between: a board
 * with something on it, for looking and for the screenshots */
void vt_demo(vt_app_t *a, int n)
{
    vt_game_t *g = &a->p[0].g;
    for (int k = 0; k < n && g->state != VT_OVER; k++) {
        while (!g->has_cur && g->state != VT_OVER) {
            vt_tick(g, 17);
            vt_events_clear(g);
        }
        if (g->state == VT_OVER) break;
        int best = 1 << 30, br = 0, bx = g->cur.x;
        for (int r = 0; r < 4; r++) {
            for (int x = -3; x < g->cols; x++) {
                if (!vt_fits(g, g->cur.type, r, x, g->cur.y)) continue;
                int y = g->cur.y;
                while (vt_fits(g, g->cur.type, r, x, y + 1)) y++;
                uint8_t col[VT_ROWS][VT_COLS_MAX];
                memcpy(col, g->col, sizeof col);
                const vt_shape_t *s = &vt_shapes[g->cur.type];
                for (int i = 0; i < s->n; i++) col[y + s->c[r][i].y][x + s->c[r][i].x] = 1;
                int sc = 0, prev = -1, full = 0;
                for (int yy = 0; yy < VT_ROWS; yy++) {
                    bool f = true;
                    int same = 0;
                    for (int xx = 0; xx < g->cols; xx++) {
                        if (!col[yy][xx]) f = false;
                        if (g->col[yy][xx] == g->cur.color) same++;
                    }
                    if (f) full++;
                    /* the bot likes to put a colour where that colour is */
                    for (int i = 0; i < s->n; i++) if (y + s->c[r][i].y == yy) sc -= same;
                }
                for (int xx = 0; xx < g->cols; xx++) {
                    int yy = 0;
                    while (yy < VT_ROWS && !col[yy][xx]) yy++;
                    int h = VT_ROWS - yy;
                    sc += h * 5;
                    if (prev >= 0) sc += (h > prev ? h - prev : prev - h) * 4;
                    prev = h;
                    for (int y2 = yy + 1; y2 < VT_ROWS; y2++) if (!col[y2][xx]) sc += 35;
                }
                sc -= full * 40;
                if (sc < best) { best = sc; br = r; bx = x; }
            }
        }
        while (g->cur.rot != br && vt_rotate(g, 1)) {}
        for (int k2 = 0; k2 < 14 && g->cur.x != bx; k2++) vt_move(g, bx > g->cur.x ? 1 : -1);
        vt_hard(g);
        vt_events_clear(g);
        for (int t = 0; t < 30; t++) {
            vt_tick(g, 17);
            vt_events_clear(g);
        }
    }
    a->nparts = 0;
    for (int i = 0; i < a->nplayers; i++) a->p[i].board_all = true;
}
#endif
