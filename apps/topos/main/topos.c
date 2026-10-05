/*
 * TOPOS - the app
 *
 * The only file that sees LVGL, the HAL and the preferences. The game itself
 * does not know any of the three exist (see topos.h).
 *
 * Living here:
 *   - presenting the dirty rectangles (flush())
 *   - the LVGL screens: the title with the three modes, pause, game over,
 *     and a banner for the countdown and the announcements
 *   - touch -> tp_game_tap(), and the pause corner
 *   - a record per mode and the sound switch, in preferences
 *
 * P4OS: the canvas is the OS's retro canvas (aos_retro.h), 180x320 shown x4:
 * the lawn is the whole 720x1280 screen. Lying down it is 320x180, the whole
 * 1280x720 (tp_geo_set()). The OS scales only the rectangles presented, by
 * hardware on the board. The game is played with the finger and nothing
 * else: the taps come from the OS as canvas coordinates, from the touch
 * panel's own samples (the moment the finger lands, which is what a
 * whack-a-mole is won by), and the pause is the score strip's left corner.
 * No OS controls are asked for: a pause pill would sit on the bottom row of
 * holes. The screens are laid out on a stage over the canvas, from its size.
 *
 * A USB gamepad plays it too, with a cursor on the holes ("The gamepad").
 *
 * Born multilingual like Claude Jump: every visible word is an LVGL label
 * wrapped in _(), and the canvas only ever gets numbers and signs.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_ui.h"
#include "aos_retro.h"
#include "aos_pad_menu.h"

#include "topos.h"
#include "tp_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Preferences, with a prefix of our own
 * -------------------------------------------------------------------------- */
#define KEY_SFX     "tp_sfx"
#define KEY_FPS     "tp_fps"
static const char *const KEY_HI[TP_MODES] = { "tp_hi0", "tp_hi1", "tp_hi2" };

#define FPS         30          /* steps a second, the watch's pace          */

/* The pause corner, in canvas pixels (200x120 on the screen). The whole
 * corner and not just the icon: the icon alone is a small target. */
#define PAUSE_W     50
#define PAUSE_H     TP_HUD_H

static const char *const MODE_NAME[TP_MODES] = {
    N_("Clásico"), N_("Supervivencia"), N_("Frenesí"),
};
static const char *const MODE_DESC[TP_MODES] = {
    N_("60 segundos"), N_("Tres vidas"), N_("30 segundos a fondo"),
};
static const uint32_t MODE_COLOR[TP_MODES] = { 0x30D158, 0xFF453A, 0xFF9F0A };

typedef struct {
    tp_game_t   g;

    lv_obj_t   *root;
    lv_obj_t   *stage;          /* the screens' parent, exactly over the canvas */
    lv_obj_t   *banner;
    const aos_retro_t *r;
    uint16_t   *bgmem;
    int         sw, sh;         /* the stage, screen px                     */

    lv_obj_t   *title, *pause, *over;
    lv_obj_t   *lbl_rec[TP_MODES];
    lv_obj_t   *lbl_snd;
    lv_obj_t   *chip_sfx, *chip_fps;
    lv_obj_t   *lbl_over_t, *lbl_over_m, *lbl_over_s, *lbl_over_b, *lbl_over_r;

    uint32_t    hi[TP_MODES];
    bool        paused;
    bool        want_exit;
    bool        leaving;        /* see app_back()                          */
    bool        closing;        /* see topos_destroy()                     */
    bool        over_shown;
    bool        over_record;    /* the result on show was a new record     */
    uint16_t    banner_ms;
    uint32_t    last_gesture_ms;
    uint16_t    auto_wait;

    uint16_t    ms_x;           /* the step's exact milliseconds: 33, 33, 34 */

    /* A USB gamepad (see "The gamepad"): its state, the hole under its
     * cursor, and the screens' buttons it goes through */
    aos_pad_t   gp;
    aos_pad_menu_t menu;
    lv_obj_t   *menu_panel;     /* the screen the menu went through last step */
    lv_obj_t   *cursor;         /* the frame round the hole, over the canvas */
    bool        cursor_on;      /* the pad has been used in play            */
    int8_t      cur_c, cur_r;
    lv_obj_t   *pm_title[4], *pm_pause[5], *pm_over[2];
} app_t;

static bool s_sfx = true;

/* --------------------------------------------------------------------------
 * Sound. tp_game.c calls it without knowing there is a HAL on the other
 * side; aos_hal_beep() queues and plays from its own task.
 * -------------------------------------------------------------------------- */
void tp_sfx(int freq_hz, int ms)
{
    if (s_sfx) {
        aos_hal_beep(freq_hz, ms);
    }
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --------------------------------------------------------------------------
 * To the screen
 *
 * tp_present() rebuilds what changed into the small buffer and lists it; we
 * upscale exactly that and invalidate exactly that. LVGL redraws by invalid
 * areas, so the saving is double.
 * -------------------------------------------------------------------------- */

static void push_rect(const tp_rect_t *r)
{
    aos_retro_present_rect(r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
}

static void flush(app_t *a)
{
    tp_game_t *g = &a->g;
    const tp_rect_t hud = { 0, 0, tp_w, TP_HUD_H };

    tp_present(g);
    for (int i = 0; i < g->push.n; i++) {
        push_rect(&g->push.r[i]);
    }
    if (g->hud_push) {
        push_rect(&hud);
    }
    g->last_area  = (uint16_t)(tp_dirty_area(&g->push) * 100 / (tp_w * tp_h));
    g->last_rects = g->push.n;
}

/* --------------------------------------------------------------------------
 * Interface pieces (Claude Jump's)
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_panel(app_t *a, lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, a->sw, a->sh);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);      /* touches do not pass through */
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* A panel whose pieces stack in a centred column: pause and the result. The
 * same code lays them out upright and lying down. */
static lv_obj_t *make_column_panel(app_t *a, lv_obj_t *parent)
{
    lv_obj_t *p = make_panel(a, parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 18, 0);
    /* centred under the score's strip, which stays readable above it */
    lv_obj_set_style_pad_top(p, TP_HUD_H * a->r->scale, 0);
    return p;
}

/* An empty gap in a column panel, on top of its row padding. */
static void make_gap(lv_obj_t *parent, int h)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, 1, h);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
}

/* y is in screen px; in a column panel the column places it instead. */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_y(l, y);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int w, int h, uint32_t accent,
                             const lv_font_t *font, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C24), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* the press shows by colour, never by transform_scale: a scale is a
     * layer, and a layer that does not fit is a hang */
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 4, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *make_chip(lv_obj_t *parent, int w, lv_event_cb_t cb, void *data)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, 76);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &aos_montserrat_28, 0);
    lv_obj_set_width(l, w - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void chip_set(lv_obj_t *chip, const char *text, bool on)
{
    if (!chip) {
        return;
    }
    lv_obj_t *l = lv_obj_get_child(chip, 0);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(on ? 0x0A0A12 : 0x9AA3B8), 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(on ? 0x30D158 : 0x1C1C24), 0);
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    int32_t v = 0;
    for (int m = 0; m < TP_MODES; m++) {
        if (aos_hal_pref_get_i32(KEY_HI[m], &v) && v > 0) {
            a->hi[m] = (uint32_t)v;
        }
    }
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
        s_sfx = v != 0;
    }
    if (aos_hal_pref_get_i32(KEY_FPS, &v)) {
        a->g.show_fps = (uint8_t)(v ? 1 : 0);
    }
}

static void prefs_save(app_t *a)
{
    for (int m = 0; m < TP_MODES; m++) {
        aos_hal_pref_set_i32(KEY_HI[m], (int32_t)a->hi[m]);
    }
    aos_hal_pref_set_i32(KEY_SFX, s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_FPS, a->g.show_fps);
}

/* --------------------------------------------------------------------------
 * Panels and the banner
 * -------------------------------------------------------------------------- */

static void overlay_hide_all(app_t *a)
{
    lv_obj_t *const panels[] = { a->title, a->pause, a->over };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* the pad has no buttons to go through (an empty set also takes the
     * outline off the one it was on) */
    aos_pad_menu_set(&a->menu, NULL, 0, 0);
    aos_retro_show_controls(true);
}

/* A panel over the lawn takes the OS's pause button away with it. */
static void overlay_show(app_t *a, lv_obj_t *panel)
{
    overlay_hide_all(a);
    if (panel) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        /* and the pad goes through its buttons */
        if (panel == a->title) {
            aos_pad_menu_set(&a->menu, a->pm_title, 4, 0);
        } else if (panel == a->pause) {
            aos_pad_menu_set(&a->menu, a->pm_pause, 5, 0);
        } else if (panel == a->over) {
            aos_pad_menu_set(&a->menu, a->pm_over, 2, 0);
        }
    }
    aos_retro_show_controls(panel == NULL && a->g.state != GS_TITLE && a->g.state != GS_OVER);
}

static void banner_hide(app_t *a)
{
    if (a->banner) {
        lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
    }
    a->banner_ms = 0;
}

/* The countdown and the announcements. A label over the canvas, not text on
 * it: it is words, and the words have accents. It changes a handful of times
 * per game, so the area it invalidates does not matter. */
static void banner_show(app_t *a, const char *text, const lv_font_t *font,
                        uint32_t color, uint16_t ms)
{
    lv_label_set_text(a->banner, text);
    lv_obj_set_style_text_font(a->banner, font, 0);
    lv_obj_set_style_text_color(a->banner, lv_color_hex(color), 0);
    lv_obj_remove_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
    a->banner_ms = ms;
}

/* --------------------------------------------------------------------------
 * Title
 * -------------------------------------------------------------------------- */

static void title_refresh(app_t *a)
{
    char buf[16];
    for (int m = 0; m < TP_MODES; m++) {
        if (a->hi[m]) {
            snprintf(buf, sizeof(buf), "%u", (unsigned)a->hi[m]);
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(a->lbl_rec[m], buf);
    }
    lv_label_set_text(a->lbl_snd, s_sfx ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
}

static void chips_refresh(app_t *a)
{
    chip_set(a->chip_sfx, _("Sonido"), s_sfx);
    chip_set(a->chip_fps, "FPS", a->g.show_fps);
}

static void go_title(app_t *a)
{
    tp_game_t *g = &a->g;
    g->state   = GS_TITLE;
    g->title_t = 0;
    a->paused     = false;
    a->over_shown = false;
    banner_hide(a);
    tp_bg_build(g, true);
    flush(a);
    overlay_show(a, a->title);
    title_refresh(a);
}

/* --------------------------------------------------------------------------
 * A game
 * -------------------------------------------------------------------------- */

static void show_end(app_t *a);

static void handle_events(app_t *a)
{
    tp_game_t *g = &a->g;
    uint16_t ev = g->events;
    char buf[48];
    g->events = 0;

    if (ev & EV_COUNT) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)g->count_num);
        banner_show(a, buf, &aos_montserrat_64, 0xFFFFFF, 700);
    }
    if (ev & EV_GO) {
        banner_show(a, _("¡Ya!"), &aos_montserrat_64, 0x30D158, 600);
    }
    if (ev & EV_LEVEL) {
        snprintf(buf, sizeof(buf), "%s %u", _("Nivel"), (unsigned)g->level);
        banner_show(a, buf, &aos_montserrat_48, 0xFFD60A, 1100);
        tp_sfx(784, 60);
        tp_sfx(988, 60);
        tp_sfx(1175, 90);
    }
    if (ev & EV_HURRY) {
        banner_show(a, _("¡Últimos 10 segundos!"), &aos_montserrat_36, 0xFF9F0A, 1300);
    }
    if (ev & EV_TIMEUP) {
        banner_show(a, _("¡Tiempo!"), &aos_montserrat_64, 0xFFFFFF, 1500);
        tp_sfx(880, 120);
        tp_sfx(660, 120);
        tp_sfx(440, 220);
    }
    if (ev & EV_DEAD) {
        banner_show(a, _("¡Sin vidas!"), &aos_montserrat_48, 0xFF453A, 1500);
        tp_sfx(392, 150);
        tp_sfx(330, 150);
        tp_sfx(262, 260);
    }
    if (ev & EV_OVER) {
        show_end(a);
    }
}

static void game_start(app_t *a, int mode)
{
    tp_game_t *g = &a->g;
    tp_game_start(g, mode);
    tp_bg_build(g, false);
    a->paused     = false;
    a->over_shown = false;
    overlay_hide_all(a);
    handle_events(a);           /* the "3" */
    flush(a);
}

static void mode0_cb(lv_event_t *e) { game_start((app_t *)lv_event_get_user_data(e), 0); }
static void mode1_cb(lv_event_t *e) { game_start((app_t *)lv_event_get_user_data(e), 1); }
static void mode2_cb(lv_event_t *e) { game_start((app_t *)lv_event_get_user_data(e), 2); }

static void pause_show(app_t *a)
{
    tp_game_t *g = &a->g;
    if (a->paused || (g->state != GS_COUNT && g->state != GS_PLAY &&
                      g->state != GS_ENDING)) {
        return;
    }
    a->paused = true;
    banner_hide(a);
    chips_refresh(a);
    overlay_show(a, a->pause);
}

static void resume_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    overlay_hide_all(a);
    a->paused  = false;
}

static void menu_cb(lv_event_t *e)
{
    go_title((app_t *)lv_event_get_user_data(e));
}

static void again_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    game_start(a, a->g.mode);
}

static void exit_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->leaving   = true;        /* so app_back() lets the system close us */
    a->want_exit = true;        /* deferred: aos_ui_back() destroys the app */
}

static void snd_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    s_sfx = !s_sfx;
    prefs_save(a);
    title_refresh(a);
    chips_refresh(a);
    tp_sfx(1200, 30);
}

static void fps_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->g.show_fps  = (uint8_t)!a->g.show_fps;
    a->g.hud_valid = 0;
    prefs_save(a);
    chips_refresh(a);
}

/* The result's labels, from the game and a->over_record. Apart from
 * show_end() because a turn of the screen builds the panel again. */
static void over_fill(app_t *a)
{
    tp_game_t *g = &a->g;
    char buf[64];
    bool record = a->over_record;

    lv_label_set_text(a->lbl_over_t, record ? _("¡Nuevo récord!") : _("¡Se acabó!"));
    lv_obj_set_style_text_color(a->lbl_over_t,
                                lv_color_hex(record ? 0xFFD60A : 0xFFFFFF), 0);
    lv_label_set_text(a->lbl_over_m, _(MODE_NAME[g->mode]));

    snprintf(buf, sizeof(buf), "%u", (unsigned)g->score);
    lv_label_set_text(a->lbl_over_s, buf);

    snprintf(buf, sizeof(buf), "%s %u   %s %u",
             _("Golpes"), (unsigned)g->hits,
             _("Mejor racha"), (unsigned)g->best_streak);
    lv_label_set_text(a->lbl_over_b, buf);

    snprintf(buf, sizeof(buf), "%s %u", _("Récord"), (unsigned)a->hi[g->mode]);
    lv_label_set_text(a->lbl_over_r, buf);
}

static void show_end(app_t *a)
{
    tp_game_t *g = &a->g;
    bool record = g->score > a->hi[g->mode];

    a->over_shown  = true;
    a->over_record = record;
    if (record) {
        a->hi[g->mode] = g->score;
        prefs_save(a);
    }
    over_fill(a);

    banner_hide(a);
    overlay_show(a, a->over);

    if (record) {
        tp_sfx(1047, 90);
        tp_sfx(1319, 90);
        tp_sfx(1568, 90);
        tp_sfx(2093, 180);
    }
}

/* --------------------------------------------------------------------------
 * Touch and gestures
 * -------------------------------------------------------------------------- */

/* The taps come from the OS (aos_retro_tap): the moment the finger LANDS,
 * from the touch panel's own samples, not LVGL's click that waits for it to
 * lift. The score strip's left corner is the pause. */
static void taps(app_t *a)
{
    int x, y;
    uint32_t down = aos_retro_pressed();
    bool tapped = aos_retro_tap(&x, &y);

    if (a->closing || a->paused || a->g.state == GS_TITLE || a->g.state == GS_OVER) {
        return;                 /* a panel is taking this finger */
    }
    /* A pad's START presses the OS's pause too, but the pad answers it
     * itself (pad_step()): taken twice, the press that resumes would pause
     * again. */
    if ((down & AOS_RETRO_BTN_PAUSE) &&
        !((a->gp.held | a->gp.released) & AOS_PAD_START)) {
        pause_show(a);
        return;
    }
    if (!tapped) {
        return;
    }
    if (x < PAUSE_W && y < PAUSE_H) {
        pause_show(a);
        return;
    }
    tp_game_tap(&a->g, x, y);
}

/* While playing a swipe is just a sloppy tap, so it does nothing. On the
 * title a swipe right leaves, as everywhere; on the result it goes back to
 * the title. Both paths (LVGL's and the touch chip's) can arrive for the same
 * swipe, hence the 400 ms. */
static bool handle_gesture(app_t *a, int dir)
{
    tp_game_t *g = &a->g;
    if (a->paused || (g->state != GS_TITLE && g->state != GS_OVER)) {
        return false;
    }
    uint32_t now = lv_tick_get();
    if ((uint32_t)(now - a->last_gesture_ms) < 400) {
        return false;
    }
    a->last_gesture_ms = now;
    if (dir != LV_DIR_RIGHT) {
        return false;
    }
    if (g->state == GS_TITLE) {
        a->want_exit = true;
    } else {
        go_title(a);
    }
    return true;
}

static void gesture_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (a->closing || !indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if ((dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) && handle_gesture(a, (int)dir)) {
        lv_indev_wait_release(indev);
    }
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * A USB pad on the board's host (aos_pad.h) plays with a cursor on the
 * holes: a white frame that only shows once the pad is used in play. The
 * d-pad (or the stick) moves it one hole at a time, and A or B whack the
 * hole under it, with the same tp_game_tap() as a finger landing there;
 * START pauses. On the screens the d-pad goes through the buttons and A
 * presses them (aos_pad_menu.h); START plays the mode under the outline
 * (Clásico if it is on the sound), resumes, and plays again at the end; B
 * is back (out of the pause, from the end to the title).
 * -------------------------------------------------------------------------- */

static void pad_click(lv_obj_t *b)
{
    if (b && lv_obj_is_valid(b)) {
        lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    }
}

/* The frame round the cursor's hole: the cell a tap counts for, a little
 * inside it so two neighbours never touch. */
static void cursor_place(app_t *a)
{
    if (!a->cursor) {
        return;
    }
    const int k = a->r->scale;
    if (a->cur_c >= tp_geo.cols) a->cur_c = (int8_t)(tp_geo.cols - 1);
    if (a->cur_r >= tp_geo.rows) a->cur_r = (int8_t)(tp_geo.rows - 1);
    int cx = TP_COL_X(a->cur_c), cy = TP_ROW_Y(a->cur_r);
    lv_obj_set_pos(a->cursor, (cx - TP_CELL_HW + 2) * k, (cy - TP_CELL_UP + 2) * k);
    lv_obj_set_size(a->cursor, (2 * TP_CELL_HW - 4) * k, (TP_CELL_UP + TP_CELL_DN - 2) * k);
}

static void cursor_show(app_t *a, bool on)
{
    if (!a->cursor) {
        return;
    }
    if (on) {
        lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pad_step(app_t *a)
{
    aos_pad_t *p = &a->gp;
    tp_game_t *g = &a->g;
    lv_obj_t *panel = a->menu.n ? lv_obj_get_parent(a->menu.item[0]) : NULL;

    aos_pad_update(p, lv_tick_get());
    /* a screen that has just come up does not take the press that brought
     * it (that START is pausing, not resuming) */
    bool fresh = panel != a->menu_panel;
    a->menu_panel = panel;

    /* the cursor is on the field only in play, and only for the pad */
    bool field = !panel && !a->paused && (g->state == GS_COUNT || g->state == GS_PLAY ||
                                         g->state == GS_ENDING);
    cursor_show(a, field && a->cursor_on);

    /* in the background, or under the app switcher, the pad is not ours */
    if (fresh || !p->connected || !lv_obj_is_visible(a->r->view)) {
        return;
    }
    if (panel) {
        if (aos_pad_menu_step(&a->menu, p)) {
            return;
        }
        bool start = aos_pad_pressed(p, AOS_PAD_START);
        bool back = aos_pad_pressed(p, AOS_PAD_B);
        if (panel == a->title) {
            if (start) {
                lv_obj_t *sel = aos_pad_menu_selected(&a->menu);
                pad_click(sel && sel != a->pm_title[3] ? sel : a->pm_title[0]);
            }
        } else if (panel == a->pause) {
            if (start || back) pad_click(a->pm_pause[0]);
        } else if (panel == a->over) {
            if (start) pad_click(a->pm_over[0]);
            else if (back) pad_click(a->pm_over[1]);
        }
        return;
    }
    if (!field) {
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        pause_show(a);
        return;
    }
    if (!a->cursor_on) {
        /* the first press only shows where the cursor is */
        if (p->pressed) {
            a->cursor_on = true;
            cursor_place(a);
            cursor_show(a, true);
        }
        return;
    }
    int c = a->cur_c, r = a->cur_r;
    if (aos_pad_repeat(p, AOS_PAD_LEFT) && c > 0) c--;
    if (aos_pad_repeat(p, AOS_PAD_RIGHT) && c < tp_geo.cols - 1) c++;
    if (aos_pad_repeat(p, AOS_PAD_UP) && r > 0) r--;
    if (aos_pad_repeat(p, AOS_PAD_DOWN) && r < tp_geo.rows - 1) r++;
    if (c != a->cur_c || r != a->cur_r) {
        a->cur_c = (int8_t)c;
        a->cur_r = (int8_t)r;
        cursor_place(a);
    }
    if (aos_pad_pressed(p, AOS_PAD_A | AOS_PAD_B)) {
        tp_game_tap(g, TP_COL_X(a->cur_c), TP_ROW_Y(a->cur_r));
    }
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

static void step(void *user)
{
    app_t *a = (app_t *)user;
    tp_game_t *g = &a->g;

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this 'a' no longer exists */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    int d = aos_ui_take_gesture();
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
        handle_gesture(a, d);
    }

    /* The OS ticks FPS times a second of real time, catching up when a frame
     * comes late, so the 60 seconds are 60 seconds: each step is exactly its
     * share, 33 or 34 ms. */
    a->ms_x = (uint16_t)(a->ms_x + 1000);
    int dt = a->ms_x / FPS;
    a->ms_x = (uint16_t)(a->ms_x % FPS);

    taps(a);
    pad_step(a);

    if (a->banner_ms) {
        if (a->banner_ms <= dt) {
            banner_hide(a);
        } else {
            a->banner_ms = (uint16_t)(a->banner_ms - dt);
        }
    }

    if (g->state == GS_TITLE) {
        g->title_t += (uint32_t)dt;
        flush(a);
        return;
    }
    if (a->paused) {
        return;
    }
    if (g->state == GS_OVER) {
        /* TP_AUTO starts over by itself, to leave it running for an hour */
        if (g->autoplay && a->over_shown && ++a->auto_wait > 60) {
            a->auto_wait = 0;
            game_start(a, g->mode);
        }
        return;
    }

    if (g->autoplay) {
        tp_game_bot(g, dt);
    }
    tp_game_step(g, dt);
    handle_events(a);
    flush(a);
}

/* Once per drawn frame: the fps the strip shows. */
static void draw(void *user)
{
    app_t *a = (app_t *)user;
    aos_retro_stats_t st;
    aos_retro_stats(&st);
    a->g.fps10 = (int16_t)st.fps10;
}

/* --------------------------------------------------------------------------
 * Back, hide
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    /* 'leaving': the Exit button goes through aos_ui_back(), which asks us
     * first. Without this we would pause, and the app could never close
     * (chatarra's trap). */
    if (!a || a->leaving) {
        return false;
    }
    switch (a->g.state) {
    case GS_COUNT:
    case GS_PLAY:
    case GS_ENDING:
        if (a->paused) {
            go_title(a);
        } else {
            pause_show(a);
        }
        return true;
    case GS_OVER:
        go_title(a);
        return true;
    default:
        return false;           /* from the title, the system leaves */
    }
}

/* Leaving mid-game should not lose the game: it pauses. */
static void topos_hide(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) {
        pause_show((app_t *)inst);
    }
}

/* --------------------------------------------------------------------------
 * Building the screens
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_mode_button(app_t *a, lv_obj_t *parent, int m,
                                  int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x15151C), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MODE_COLOR[m]), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(MODE_COLOR[m]), 0);
    lv_obj_set_style_border_width(b, 4, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, a);

    /* Name and description on the left, record on the right. Fixed boxes
     * with an ellipsis: these are absolute positions, and German grows. */
    const int rec_w = 170, text_w = w - 28 - rec_w - 36;
    lv_obj_t *n = lv_label_create(b);
    lv_label_set_text(n, _(MODE_NAME[m]));
    lv_obj_set_style_text_font(n, &aos_montserrat_36, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(n, 28, h / 2 - 50);
    lv_obj_set_width(n, text_w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *d = lv_label_create(b);
    lv_label_set_text(d, _(MODE_DESC[m]));
    lv_obj_set_style_text_font(d, &aos_montserrat_24, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(0xA0A8B8), 0);
    lv_obj_set_pos(d, 28, h / 2 + 8);
    lv_obj_set_width(d, text_w);
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *r = lv_label_create(b);
    lv_label_set_text(r, "");
    lv_obj_set_style_text_font(r, &aos_montserrat_36, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0xFFD60A), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(r, rec_w);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    a->lbl_rec[m] = r;
    return b;
}

/* The title. The big mole, the bomb and the mallet are the canvas
 * (tp_geo.title_x/y); what goes round them is laid out from where the canvas
 * puts them, so it holds at any scale. Upright: the name above the scene,
 * the three modes under it and how to play at the bottom. Lying down: the
 * name, the scene and how to play on the left, the modes on the right. */
static void build_title(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_panel(a, root);
    a->title = p;

    const int k  = a->r->scale;
    const int W  = a->sw, H = a->sh;
    const bool land = tp_geo.land;
    const int sx = tp_geo.title_x * k;              /* the big hole, on screen */
    const int top = (tp_geo.title_y - 64) * k;      /* the hat's top           */
    const int bot = (tp_geo.title_y + 22) * k;      /* the mound's bottom      */
    const int left_w = land ? sx * 2 : W;           /* the scene's column      */

    /* The name, with a dark shadow under it: white alone does not read on
     * sunlit grass. */
    const int ty = land ? top / 2 - 36 : top / 2 - 30;
    const int tw = left_w - 80;
    lv_obj_t *sh = make_label(p, _("Topos"), &aos_montserrat_64, 0x1E4A12, ty + 4);
    lv_obj_set_width(sh, tw);
    lv_obj_set_x(sh, sx - tw / 2 + 3);
    lv_label_set_long_mode(sh, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *t = make_label(p, _("Topos"), &aos_montserrat_64, 0xFFFFFF, ty);
    lv_obj_set_width(t, tw);
    lv_obj_set_x(t, sx - tw / 2);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *snd = make_button(p, LV_SYMBOL_VOLUME_MAX, W - 96 - 32, 32, 96, 84,
                                0x8E8E93, &aos_montserrat_36, snd_cb, a);
    a->lbl_snd = lv_obj_get_child(snd, 0);

    /* the modes: a column under the scene, or on the right half */
    const int bh = 124, gap = 24;
    int bx, by, bw;
    if (land) {
        bx = left_w + 24;
        bw = W - bx - 40;
        by = (H - 3 * bh - 2 * gap) / 2 + 20;
    } else {
        bw = W - 2 * 56;
        bx = 56;
        by = bot + 48;
    }
    a->pm_title[0] = make_mode_button(a, p, MODE_CLASSIC,  bx, by,                  bw, bh, mode0_cb);
    a->pm_title[1] = make_mode_button(a, p, MODE_SURVIVAL, bx, by + bh + gap,       bw, bh, mode1_cb);
    a->pm_title[2] = make_mode_button(a, p, MODE_FRENZY,   bx, by + 2 * (bh + gap), bw, bh, mode2_cb);
    a->pm_title[3] = snd;

    /* How to play: read, not touched. Under the modes upright, under the
     * scene lying down. */
    const int hw = land ? left_w - 96 : bw;
    const int hy = land ? bot + 36 : by + 3 * bh + 2 * gap + 44;
    lv_obj_t *h = make_label(p, _("Tocá los topos; los de casco, dos veces. ¡Las bombas no!"),
                             &aos_montserrat_28, 0xFFFFFF, hy);
    lv_obj_set_width(h, hw);
    lv_obj_set_x(h, land ? sx - hw / 2 : bx);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_40, 0);
    lv_obj_set_style_radius(h, 22, 0);
    lv_obj_set_style_pad_hor(h, 20, 0);
    lv_obj_set_style_pad_ver(h, 12, 0);
}

static void row_ext_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, 12);     /* aos_pad_menu.h's outline: 4 + 5 */
}

static void build_pause(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_column_panel(a, root);
    a->pause = p;

    make_label(p, _("Pausa"), &aos_montserrat_64, 0xFFFFFF, 0);
    make_gap(p, 12);
    a->pm_pause[0] = make_button(p, _("Seguir"), 0, 0, 480, 108, 0x30D158,
                                 &aos_montserrat_48, resume_cb, a);
    a->pm_pause[1] = make_button(p, _("Menú"), 0, 0, 480, 88, 0x0A84FF,
                                 &aos_montserrat_36, menu_cb, a);
    a->pm_pause[2] = make_button(p, _("Salir"), 0, 0, 480, 88, 0xFF453A,
                                 &aos_montserrat_36, exit_cb, a);

    /* the two switches side by side */
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 480, 76);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    a->chip_sfx = make_chip(row, 230, snd_cb, a);
    a->chip_fps = make_chip(row, 230, fps_cb, a);
    lv_obj_align(a->chip_sfx, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(a->chip_fps, LV_ALIGN_RIGHT_MID, 0, 0);
    /* the pad's outline goes round a chip, past the row's edge: the row
     * lets it show and redraws that far */
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(row, row_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(row);
    a->pm_pause[3] = a->chip_sfx;
    a->pm_pause[4] = a->chip_fps;
}

static void build_over(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_column_panel(a, root);
    a->over = p;
    lv_obj_set_style_pad_row(p, 10, 0);

    a->lbl_over_t = make_label(p, "", &aos_montserrat_48, 0xFFFFFF, 0);
    a->lbl_over_m = make_label(p, "", &aos_montserrat_28, 0x9AA3B8, 0);
    a->lbl_over_s = make_label(p, "", &aos_montserrat_64, 0xFFFFFF, 0);
    a->lbl_over_b = make_label(p, "", &aos_montserrat_28, 0xDDE3EE, 0);
    a->lbl_over_r = make_label(p, "", &aos_montserrat_28, 0xFFD60A, 0);
    make_gap(p, 24);
    a->pm_over[0] = make_button(p, _("Otra vez"), 0, 0, 480, 108, 0x30D158,
                                &aos_montserrat_48, again_cb, a);
    make_gap(p, 4);
    a->pm_over[1] = make_button(p, _("Menú"), 0, 0, 480, 88, 0x0A84FF,
                                &aos_montserrat_36, menu_cb, a);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void free_buffers(app_t *a)
{
    aos_retro_end();
    free(a->bgmem);
    a->bgmem = NULL;
    tp_art_free();
}

/* The canvas for the root's shape, and every LVGL screen on a stage over it.
 * From create(), and again from resize() when the screen turns. */
static bool ui_build(app_t *a, lv_obj_t *root)
{
    lv_obj_update_layout(root);
    tp_geo_set(lv_obj_get_width(root) > lv_obj_get_height(root));

    /* Only taps: no pad and no OS pause button, so the canvas takes the
     * biggest factor that fits the whole root (x4 full screen), centred. */
    a->r = aos_retro_begin(root, tp_w, tp_h, 0, AOS_RETRO_TOUCH | AOS_RETRO_CENTER);
    if (!a->r) {
        return false;
    }
    tp_buf_init(&a->g.fb, a->r->px, tp_w, tp_h);
    tp_buf_init(&a->g.bg, a->bgmem, tp_w, tp_h);

    /* the screens live on a stage exactly over the canvas */
    a->sw = a->r->w * a->r->scale;
    a->sh = a->r->h * a->r->scale;
    a->stage = lv_obj_create(root);
    lv_obj_remove_style_all(a->stage);
    lv_obj_set_pos(a->stage, a->r->x, a->r->y);
    lv_obj_set_size(a->stage, a->sw, a->sh);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_CLICKABLE);

    a->banner = lv_label_create(a->stage);
    lv_label_set_text(a->banner, "");
    lv_obj_set_style_bg_color(a->banner, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->banner, LV_OPA_60, 0);
    lv_obj_set_style_radius(a->banner, 36, 0);
    lv_obj_set_style_pad_hor(a->banner, 36, 0);
    lv_obj_set_style_pad_ver(a->banner, 12, 0);
    lv_obj_set_style_text_align(a->banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_width(a->banner, a->sw * 88 / 100, 0);
    lv_label_set_long_mode(a->banner, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(a->banner, LV_ALIGN_CENTER, 0, -a->sh / 16);
    lv_obj_remove_flag(a->banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);

    /* the pad's cursor, under the screens: hidden until the pad plays */
    a->cursor = lv_obj_create(a->stage);
    lv_obj_remove_style_all(a->cursor);
    lv_obj_set_style_radius(a->cursor, 10 * a->r->scale, 0);
    lv_obj_set_style_border_color(a->cursor, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->cursor, a->r->scale + 2, 0);
    lv_obj_set_style_outline_color(a->cursor, lv_color_hex(0x000000), 0);
    lv_obj_set_style_outline_opa(a->cursor, LV_OPA_50, 0);
    lv_obj_set_style_outline_width(a->cursor, 3, 0);
    lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);
    cursor_place(a);

    build_title(a, a->stage);
    build_pause(a, a->stage);
    build_over(a, a->stage);

    aos_retro_run(FPS, step, draw, a);
    return true;
}

static void *topos_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }

    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("topos", "opening | internal %u B, psram %u B",
                (unsigned)heap_int, (unsigned)heap_psram);

    /* The frame is the OS's canvas; the lawn behind it is ours (malloc sends
     * it to PSRAM), the same size whichever way up, plus ~60 KB of sprites. */
    a->bgmem = (uint16_t *)malloc((size_t)TP_PIXELS * sizeof(uint16_t));
    uint64_t t0 = aos_hal_uptime_ms();
    if (!a->bgmem || !tp_art_init() || !ui_build(a, root)) {
        aos_hal_log("topos", "out of memory for the canvas or the sprites");
        free_buffers(a);
        lv_free(a);
        return NULL;
    }
    aos_hal_log("topos", "sprites rendered in %u ms",
                (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));

    tp_game_init(&a->g, (uint32_t)aos_hal_uptime_ms());
    prefs_load(a);

    a->root = root;

    /* The gesture is listened for on the ROOT with GESTURE_BUBBLE taken off:
     * LVGL hands it to the first ancestor without the flag. */
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, a);

    go_title(a);

#ifdef AOS_SIM_BUILTIN
    /* Development switches. On the board getenv() always returns NULL.
     *
     *   TP_AUTO=1         the bot plays, and starts over after each game
     *   TP_MODE=0|1|2     straight into that mode
     *   TP_FPS=1          frames per second in the score's strip
     *   TP_REC=500        fake records, to see them on the title
     *   TP_SCREEN=pause|over   straight into that panel: the layout audit
     *                     skips hidden objects, and both are born hidden
     */
    {
        const char *env;
        int mode = -1;
        if ((env = getenv("TP_FPS")) && env[0]) {
            a->g.show_fps = 1;
        }
        if ((env = getenv("TP_REC")) && env[0]) {
            for (int m = 0; m < TP_MODES; m++) {
                a->hi[m] = (uint32_t)atoi(env) * (uint32_t)(m + 1);
            }
            title_refresh(a);
        }
        if ((env = getenv("TP_MODE")) && env[0]) {
            mode = clampi(atoi(env), 0, TP_MODES - 1);
        }
        if ((env = getenv("TP_AUTO")) && env[0]) {
            a->g.autoplay = 1;
            if (mode < 0) {
                mode = MODE_CLASSIC;
            }
        }
        if ((env = getenv("TP_SCREEN")) && env[0]) {
            if (mode < 0) {
                mode = MODE_CLASSIC;
            }
        }
        if (mode >= 0) {
            game_start(a, mode);
        }
        if ((env = getenv("TP_SCREEN")) && env[0]) {
            if (env[0] == 'p') {
                pause_show(a);
            } else if (env[0] == 'o') {
                a->g.score       = 1234;
                a->g.hits        = 57;
                a->g.best_streak = 14;
                a->g.state       = GS_OVER;
                show_end(a);
            }
        }
    }
#endif

    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("topos", "ready | internal %u B, psram %u B | canvas %dx%d x%d, %d holes",
                (unsigned)heap_int, (unsigned)heap_psram, tp_w, tp_h, a->r->scale,
                tp_geo.holes);
    return a;
}

static void topos_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    aos_retro_stop();

    /* The objects are deleted HERE and not left to the runtime: it calls
     * destroy() and only then deletes the root, so LV_EVENT_PRESS_LOST from
     * a finger still down would reach touch_cb with the context freed. */
    a->closing = true;
    if (a->root) {
        lv_obj_clean(a->root);
    }

    prefs_save(a);
    aos_hal_log("topos", "closing | records %u / %u / %u",
                (unsigned)a->hi[0], (unsigned)a->hi[1], (unsigned)a->hi[2]);

    free_buffers(a);
    lv_free(a);
}

/* The screen turned. The field is another shape (fifteen holes upright, ten
 * lying down), so what is out of the holes cannot move across; everything
 * else does. A game in progress keeps its score, clock, hearts and streak
 * and waits in pause on the new field; the title and the result come back
 * as they were. */
static bool topos_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    tp_game_t *g = &a->g;

    aos_retro_stop();
    aos_retro_end();
    aos_pad_menu_set(&a->menu, NULL, 0, 0);     /* its buttons are about to go */
    a->menu_panel = NULL;
    a->closing = true;          /* no callback of ours while they go */
    lv_obj_clean(root);
    a->closing = false;
    a->title = a->pause = a->over = a->banner = a->stage = a->cursor = NULL;
    a->chip_sfx = a->chip_fps = NULL;
    if (!ui_build(a, root)) {
        return false;           /* the runtime creates the app again */
    }

    tp_game_clear_field(g);
    a->banner_ms = 0;
    title_refresh(a);
    if (g->state == GS_TITLE) {
        go_title(a);
        return true;
    }
    tp_bg_build(g, false);
    flush(a);
    if (g->state == GS_OVER) {
        if (a->over_shown) {
            over_fill(a);
            overlay_show(a, a->over);
        }
        return true;
    }
    a->paused = false;
    overlay_hide_all(a);
    pause_show(a);
    return true;
}

/* The launcher icon: a mole peeking out of its hole. Percent coordinates,
 * cardinal shapes, the pink nose the only colour of its own. */
static const uint8_t TOPOS_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  -4, 44, 52, 22,         AIC_C_TEXT,          255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, -8,  12,  6,  8, AIC_CIRCLE, AIC_C_LIT(0x000000), 255),
    AIC_RECT(AIC_TOP_MID,  8,  12,  6,  8, AIC_CIRCLE, AIC_C_LIT(0x000000), 255),
    AIC_RECT(AIC_TOP_MID,  0,  22, 13,  9, AIC_CIRCLE, AIC_C_LIT(0xFF8FA6), 255),
    AIC_OUT,
    AIC_RECT(AIC_CENTER,   0,  25, 76, 20, AIC_CIRCLE, AIC_C_LIT(0xDDA05E), 255),
    AIC_END
};

static bool topos_init(aos_app_t *app)
{
    app->desc.id       = "demo.topos";
    app->desc.name     = "Topos";
    app->desc.icon     = LV_SYMBOL_PLAY;
    /* The icon travels with the app (docs/ICONS.md): the same mole the
     * firmware drew as AOS_ICON_MOLE, now described here and copied by the
     * runtime at load. icon_vec stays NONE so that the glyph above is the
     * only fallback, should the blob ever be refused. */
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, TOPOS_ICON, sizeof TOPOS_ICON);
    /* Lawn over dirt. Darkish at both ends: the icon's shape is drawn in
     * white and the colour is the app's. */
    app->desc.color_a  = 0x3E8E2E;
    app->desc.color_b  = 0x6B4020;
    app->desc.order    = 149;           /* among the games */
    /* Either way up: the field is laid out for the screen's shape. */
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE;

    app->create  = topos_create;
    app->destroy = topos_destroy;
    app->resize  = topos_resize;
    app->hide    = topos_hide;
    app->back    = app_back;
    return true;
}

AOS_APP_ENTRY(topos_init);
