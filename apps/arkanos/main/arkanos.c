/*
 * ARKANOS - brick breaking, on the P4OS retro canvas
 *
 * An homage to Arkanoid: paddle, ball, twelve walls and falling capsules. It
 * builds two ways from the same source:
 *
 *   .so for the board               apps/arkanos (project_so)
 *   built-in app of the simulator   the simulator builds it (AOS_SIM_BUILTIN)
 *
 * What changed from the watch (AmoledOS apps/arkanos):
 *
 *  - The canvas is the OS's (aos_retro.h) and fills the screen: 240x426 x3 in
 *    portrait, 426x240 x3 in landscape, chosen from the root's shape (and
 *    again in resize() when the screen turns, keeping the game). The game
 *    still draws with the dirty-rectangle list of ak_draw.c, straight into
 *    the service's buffer, and presents only the union of the rectangles.
 *  - It is played by touch alone. The paddle goes to the column of the
 *    finger, anywhere on the field or -the comfortable way, the finger does
 *    not cover the ball- on the deck drawn under the paddle in portrait.
 *    Lifting the finger launches the ball; with the laser fitted, a finger
 *    on the glass fires. Touching the score (portrait) or the II button
 *    (landscape) pauses.
 *  - The pace is the service's fixed tick (30 steps a second).
 *  - A USB gamepad plays it too ("The gamepad", below).
 *
 * The record and the switches are the same preferences as on the watch.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_retro.h"
#include "aos_pad_menu.h"

#include "arkanos.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_HI      "ak_hi"
#define KEY_SFX     "ak_sfx"
#define KEY_FPS     "ak_fps"

static bool s_sfx = true;

void ak_sfx(int freq_hz, int ms)
{
    if (s_sfx) {
        aos_hal_beep(freq_hz, ms);
    }
}

typedef struct {
    ak_t        g;
    const aos_retro_t *r;
    uint16_t   *bgmem;      /* the background with the bricks, AK_PX_MAX */

    lv_obj_t   *stage;      /* the panels' parent: exactly over the canvas */

    /* panels */
    lv_obj_t   *title;
    lv_obj_t   *title_hi;
    lv_obj_t   *chip_sfx;
    lv_obj_t   *chip_fps;
    lv_obj_t   *pause;
    lv_obj_t   *chip_sfx2;
    lv_obj_t   *chip_fps2;
    lv_obj_t   *over;
    lv_obj_t   *over_title;
    lv_obj_t   *over_score;
    lv_obj_t   *over_go;        /* the green button: retry or new lap */
    uint8_t     over_shown;
    uint8_t     won;
    uint8_t     paused_from;    /* the state the pause interrupted */
    bool        finger;         /* a finger was on the canvas in the last step */

    /* A USB gamepad (see "The gamepad"): its state, the panel's buttons it
     * goes through, and those buttons per panel */
    aos_pad_t   gp;
    aos_pad_menu_t menu;
    uint8_t     gp_run;         /* steps the d-pad has been held one way */
    lv_obj_t   *pm_title[4], *pm_pause[4], *pm_over[3];

    bool        want_exit;
} app_t;

/* --------------------------------------------------------------------------
 * To the screen
 * -------------------------------------------------------------------------- */


static void push_rect(const ak_rect_t *r)
{
    aos_retro_present_rect(r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
}

static void push_all(app_t *a)
{
    aos_retro_present();
    a->g.last_area = 100;
}

static void present(app_t *a)
{
    ak_t *g = &a->g;
    const ak_rect_t ak_entera = { 0, 0, AK_W, AK_H };

    /* 1. restore from the background whatever we dirtied last frame, plus
     *    whatever changed in the background itself (broken or worn bricks) */
    g->d_push = g->d_prev;
    ak_dirty_join(&g->d_push, &g->d_bg);

    if (g->d_push.all) {
        ak_restore(g->fb.px, a->bgmem, &ak_entera);
        g->hud_dirty = 1;       /* the score went with it */
    } else {
        for (int i = 0; i < g->d_push.n; i++) {
            ak_restore(g->fb.px, a->bgmem, &g->d_push.r[i]);
        }
    }

    /* 2. draw what moves; fills d_cur */
    ak_draw_movers(g);

    /* 3. the score, only if a number changed */
    if (g->hud_dirty) {
        ak_draw_hud(g);
    }

    /* 4. present the union: the OS scales only that */
    ak_dirty_join(&g->d_push, &g->d_cur);

    if (g->d_push.all) {
        push_all(a);
    } else {
        for (int i = 0; i < g->d_push.n; i++) {
            push_rect(&g->d_push.r[i]);
        }
        g->last_area = (uint16_t)(ak_dirty_area(&g->d_push) * 100 / (AK_W * AK_H));
    }

    g->d_prev = g->d_cur;
    ak_dirty_reset(&g->d_bg);
}

/* --------------------------------------------------------------------------
 * LVGL panels
 * -------------------------------------------------------------------------- */

static void controls_for(app_t *a, lv_obj_t *panel)
{
    /* over a menu the pad has nothing to do: it goes away with the panel */
    aos_retro_show_controls(panel == NULL);
    (void)a;
}

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
    controls_for(a, NULL);
}

static void overlay_show(app_t *a, lv_obj_t *panel)
{
    overlay_hide_all(a);
    if (panel) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        /* and the pad goes through its buttons, from the first one */
        if (panel == a->title) {
            aos_pad_menu_set(&a->menu, a->pm_title, 4, 0);
        } else if (panel == a->pause) {
            aos_pad_menu_set(&a->menu, a->pm_pause, 4, 0);
        } else if (panel == a->over) {
            aos_pad_menu_set(&a->menu, a->pm_over, 3, 0);
        }
    }
    controls_for(a, panel);
}

/* The panels are LVGL cards over the canvas, a column centred on the stage:
 * the flex layout lets the same code serve portrait and landscape. */
static lv_obj_t *make_panel(lv_obj_t *parent, int w, int gap)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, gap, 0);
    lv_obj_set_style_pad_top(p, gap + 12, 0);
    lv_obj_set_style_pad_bottom(p, gap + 12, 0);
    lv_obj_set_style_pad_left(p, 24, 0);
    lv_obj_set_style_pad_right(p, 24, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_style_radius(p, 36, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_center(p);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);      /* so touches do not pass through */
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static void row_ext_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, 12);     /* aos_pad_menu.h's outline: 4 + 5 */
}

/* A row inside a panel: chips or buttons side by side. */
static lv_obj_t *make_row(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    /* the pad's outline goes round a button, past the row's edge: the row
     * lets it show and redraws that far */
    lv_obj_add_flag(r, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(r, row_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(r);
    return r;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, const char *sub,
                             int w, int h, uint32_t accent,
                             lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C24), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* The press highlight goes by colour and not by transform_scale: scaling
     * forces LVGL to build a separate layer. */
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, sub ? &aos_montserrat_36 : &aos_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_align(l, LV_ALIGN_CENTER, 0, sub ? -16 : 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);

    if (sub) {
        lv_obj_t *s = lv_label_create(b);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_font(s, &aos_montserrat_24, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9AA3B8), 0);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s, lv_pct(100));
        lv_obj_align(s, LV_ALIGN_CENTER, 0, 26);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    }
    return b;
}

/* Chips: flat buttons whose text states the setting. */
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

static lv_obj_t *make_chip(lv_obj_t *parent, int w, lv_event_cb_t cb, void *data)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, 64);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &aos_montserrat_24, 0);
    /* the text is clipped INSIDE the chip, width and height: a long
     * translation looks ugly but stays contained */
    lv_obj_set_size(l, w - 16, 32);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void prefs_save(app_t *a)
{
    aos_hal_pref_set_i32(KEY_SFX, s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_FPS, a->g.show_fps);
}

static void chips_refresh(app_t *a)
{
    chip_set(a->chip_sfx,  s_sfx ? _("SONIDO SI") : _("SONIDO NO"), s_sfx);
    chip_set(a->chip_sfx2, s_sfx ? _("SONIDO SI") : _("SONIDO NO"), s_sfx);
    chip_set(a->chip_fps,  a->g.show_fps ? _("FPS SI") : _("FPS NO"), a->g.show_fps);
    chip_set(a->chip_fps2, a->g.show_fps ? _("FPS SI") : _("FPS NO"), a->g.show_fps);
}

static void title_refresh(app_t *a)
{
    char buf[32];
    snprintf(buf, sizeof(buf), _("RECORD  %lu"), (unsigned long)a->g.hiscore);
    lv_label_set_text(a->title_hi, buf);
    chips_refresh(a);
}

static void start_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->over_shown = 0;
    prefs_save(a);
    overlay_hide_all(a);
    ak_game_start(&a->g);
}

static void chip_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_obj_t *chip = lv_event_get_target_obj(event);

    if (chip == a->chip_sfx || chip == a->chip_sfx2) {
        s_sfx = !s_sfx;
    } else if (chip == a->chip_fps || chip == a->chip_fps2) {
        a->g.show_fps = !a->g.show_fps;
        /* the number is drawn over the field: what was there has to be erased */
        ak_dirty_all(&a->g.d_bg);
    }
    prefs_save(a);
    chips_refresh(a);
    aos_hal_beep(1200, 20);
}

static void pause_show(app_t *a)
{
    if (a->g.state == ST_PLAY || a->g.state == ST_READY ||
        a->g.state == ST_LOST || a->g.state == ST_CLEAR) {
        a->paused_from = a->g.state;
        a->g.state = ST_PAUSE;
        a->g.fire_down = 0;
        a->g.fire_edge = 0;
        a->g.touching = 0;
        chips_refresh(a);
        overlay_show(a, a->pause);
    }
}

static void resume_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    overlay_hide_all(a);
    /* back to where it was: resuming a lost ball as PLAY would find no ball
     * alive and take a second life */
    a->g.state = a->paused_from ? a->paused_from : ST_PLAY;
    a->g.fire_edge = 0;
    /* the canvas was covered by the panel: it has to be rebuilt whole */
    ak_dirty_all(&a->g.d_bg);
    a->g.hud_dirty = 1;
}

/* The panel's green button at the end. One button for both endings: on
 * finishing the twelve levels you go back to the first more quickly, as in the
 * arcades (the score carries on and the lap shows in the HUD, "N1-2"), and if
 * it was a defeat you start again. */
static void green_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->over_shown = 0;
    overlay_hide_all(a);
    if (a->won) {
        ak_load_level(&a->g, ak_level_count());
    } else {
        ak_game_start(&a->g);
    }
}

static void menu_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->over_shown = 0;
    a->g.state = ST_TITLE;
    a->g.state_t = 0;
    title_refresh(a);
    overlay_show(a, a->title);
}

static void exit_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->want_exit = true;        /* it exits on the next frame, not here */
}

/* Room between the rows of a panel: landscape has 720 px of height for the
 * title's nine rows, portrait 1278. */
static int panel_gap(void)
{
    return ak_geo.land ? 8 : 20;
}

static void build_title(app_t *a, lv_obj_t *root)
{
    bool land = ak_geo.land;
    int w = land ? 720 : 648;
    lv_obj_t *p = make_panel(root, w, panel_gap());
    a->title = p;
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05060F), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_90, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x2A3145), 0);

    make_label(p, _("ARKANOS"), &aos_montserrat_64, 0xFF9F0A);
    make_label(p, _("DOCE MUROS Y UNA BOLA"), &aos_montserrat_24, 0xFFE45E);

    lv_obj_t *go = make_button(p, _("JUGAR"), _("arrastra el dedo"),
                               w - 96, land ? 104 : 128, 0x0A84FF, start_cb, a);
    lv_obj_set_style_margin_top(go, land ? 4 : 12, 0);
    a->pm_title[0] = go;
    lv_obj_set_style_margin_bottom(go, land ? 4 : 12, 0);

    /* How it is played, as it really is on this screen: by touch alone. */
    lv_obj_t *how = lv_obj_create(p);
    lv_obj_remove_style_all(how);
    lv_obj_set_size(how, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(how, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(how, land ? 2 : 8, 0);
    lv_obj_remove_flag(how, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(how, LV_OBJ_FLAG_SCROLLABLE);
    make_label(how, land ? _("EL DEDO SOBRE LA CANCHA MUEVE LA PALETA")
                         : _("EL DEDO EN LA FRANJA DE ABAJO MUEVE LA PALETA"),
               &aos_montserrat_20, 0x7BE9FF);
    make_label(how, _("SUELTA EL DEDO PARA SACAR"), &aos_montserrat_20, 0x7BE9FF);
    make_label(how, _("CON DISPARO, EL DEDO APOYADO DISPARA"), &aos_montserrat_20, 0x7BE9FF);
    make_label(how, land ? _("EL BOTON II DE LA DERECHA PAUSA")
                         : _("TOCA EL MARCADOR PARA PAUSAR"),
               &aos_montserrat_20, 0x8A8A98);

    lv_obj_t *row = make_row(p, 16);
    lv_obj_set_style_margin_top(row, land ? 4 : 8, 0);
    a->chip_sfx = make_chip(row, (w - 96 - 16) / 2, chip_cb, a);
    a->chip_fps = make_chip(row, (w - 96 - 16) / 2, chip_cb, a);

    a->title_hi = make_label(p, _("RECORD 0"), &aos_montserrat_28, 0xFFFFFF);
    a->pm_title[1] = a->chip_sfx;
    a->pm_title[2] = a->chip_fps;
    a->pm_title[3] = make_button(p, _("SALIR"), NULL, 220, 68, 0xFF453A, exit_cb, a);
}

static void build_pause(app_t *a, lv_obj_t *root)
{
    int w = 520;
    lv_obj_t *p = make_panel(root, w, panel_gap() + 6);
    a->pause = p;

    make_label(p, _("PAUSA"), &aos_montserrat_48, 0xFFFFFF);
    a->pm_pause[0] = make_button(p, _("SEGUIR"), NULL, w - 64, 96, 0x30D158, resume_cb, a);
    a->chip_sfx2 = make_chip(p, w - 64, chip_cb, a);
    a->chip_fps2 = make_chip(p, w - 64, chip_cb, a);
    lv_obj_t *out = make_button(p, _("SALIR"), NULL, w - 64, 80, 0xFF453A, exit_cb, a);
    lv_obj_set_style_margin_top(out, 12, 0);
    a->pm_pause[1] = a->chip_sfx2;
    a->pm_pause[2] = a->chip_fps2;
    a->pm_pause[3] = out;
}

static void build_over(app_t *a, lv_obj_t *root)
{
    int w = 560;
    lv_obj_t *p = make_panel(root, w, panel_gap() + 6);
    a->over = p;
    lv_obj_set_style_border_color(p, lv_color_hex(0xFF9F0A), 0);

    a->over_title = make_label(p, _("FIN DEL JUEGO"), &aos_montserrat_48, 0xFF4A3D);
    a->over_score = make_label(p, "", &aos_montserrat_28, 0xFFFFFF);

    a->over_go = make_button(p, _("OTRA VEZ"), NULL, w - 64, 96, 0x30D158, green_cb, a);
    lv_obj_set_style_margin_top(a->over_go, 12, 0);
    lv_obj_t *row = make_row(p, 16);
    a->pm_over[0] = a->over_go;
    a->pm_over[1] = make_button(row, _("MENU"), NULL, (w - 64 - 16) / 2, 80, 0x0A84FF, menu_cb, a);
    a->pm_over[2] = make_button(row, _("SALIR"), NULL, (w - 64 - 16) / 2, 80, 0xFF453A, exit_cb, a);
}

/* Both endings use the same panel: the title changes and so does what the
 * green button does. */
static void show_end(app_t *a, bool gano)
{
    char buf[48];

    a->won = gano ? 1 : 0;
    lv_label_set_text(a->over_title, gano ? _("TERMINASTE") : _("FIN DEL JUEGO"));
    lv_obj_set_style_text_color(a->over_title,
                                lv_color_hex(gano ? 0x30D158 : 0xFF4A3D), 0);

    snprintf(buf, sizeof(buf), _("%lu  (RECORD %lu)"),
             (unsigned long)a->g.score, (unsigned long)a->g.hiscore);
    lv_label_set_text(a->over_score, buf);
    lv_label_set_text(lv_obj_get_child(a->over_go, 0),
                      gano ? _("OTRA VUELTA") : _("OTRA VEZ"));

    aos_hal_pref_set_i32(KEY_HI, (int32_t)a->g.hiscore);
    overlay_show(a, a->over);
    a->over_shown = 1;
}

/* --------------------------------------------------------------------------
 * The frame: input, one step of the simulation (30 a second), and the draw
 * -------------------------------------------------------------------------- */

static bool playing(const ak_t *g)
{
    return g->state == ST_PLAY || g->state == ST_READY ||
           g->state == ST_LOST || g->state == ST_CLEAR;
}

static bool in_pause(int x, int y)
{
    const ak_rect_t *p = &ak_geo.pause;
    return x >= p->x0 && x < p->x1 && y >= p->y0 && y < p->y1;
}

static void read_input(app_t *a)
{
    ak_t *g = &a->g;
    int tx, ty;
    bool touching = aos_retro_touch(&tx, &ty);
    int ux, uy;
    bool tapped = aos_retro_tap(&ux, &uy);

    if (!playing(g)) {
        g->touching = 0;
        g->fire_down = 0;
        a->finger = false;
        return;
    }
    /* a finger landing on the score (portrait) or on II (landscape) */
    if (tapped && in_pause(ux, uy)) {
        aos_hal_beep(700, 30);
        pause_show(a);
        return;
    }

    /* the finger: the middle of the canvas pixel it is on, in 1/16. Only x
     * counts; the paddle clamps it to the field */
    if (touching) {
        g->touch_x = (int16_t)(tx * FX_ONE + FX_ONE / 2);
        g->touch_y = (int16_t)(ty * FX_ONE + FX_ONE / 2);
    } else if (a->finger || tapped) {
        /* lifted, or a tap already over within one step: the ball goes.
         * Launching on the lift and not on the landing lets the thumb come
         * down, aim, and only then serve. */
        g->fire_edge = 1;
    }
    a->finger = touching;
    g->touching = touching ? 1 : 0;
    g->fire_down = (touching || tapped) ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * A USB pad on the board's host (aos_pad.h) plays alongside the finger. The
 * d-pad moves the paddle, slow for the first steps so it can be placed to
 * the pixel and then faster; the left stick moves it at a speed that
 * follows how far it leans. A serves the ball, and with the laser fitted
 * fires while held; START pauses. Over a panel the d-pad goes through its
 * buttons and A presses them (aos_pad_menu.h); START on the title plays, on
 * the pause it resumes and on the end panel it plays again, and B is back:
 * out of the pause, or from the end panel to the menu.
 * -------------------------------------------------------------------------- */

#define PAD_DEAD        6000        /* the stick's dead zone, of 32767 */
#define PAD_STICK_MAX   14          /* px a step with the stick all the way */
#define PAD_DPAD_MIN    4           /* px a step on the d-pad: at first... */
#define PAD_DPAD_MAX    11          /* ...and after a few steps held */

static void pad_click(lv_obj_t *b)
{
    if (b && lv_obj_is_valid(b)) {
        lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    }
}

static void pad_step(app_t *a)
{
    ak_t *g = &a->g;
    aos_pad_t *p = &a->gp;

    aos_pad_update(p, lv_tick_get());
    /* in the background, or under the app switcher, the pad is not ours */
    if (!p->connected || !lv_obj_is_visible(a->r->view)) {
        return;
    }
    if (a->menu.n) {
        /* a panel is up */
        if (aos_pad_menu_step(&a->menu, p)) {
            return;
        }
        bool start = aos_pad_pressed(p, AOS_PAD_START);
        bool back = aos_pad_pressed(p, AOS_PAD_B);
        if (a->menu.item[0] == a->pm_title[0] && start) {
            pad_click(a->pm_title[0]);
        } else if (a->menu.item[0] == a->pm_pause[0] && (start || back)) {
            pad_click(a->pm_pause[0]);
        } else if (a->menu.item[0] == a->pm_over[0]) {
            if (start) pad_click(a->pm_over[0]);
            else if (back) pad_click(a->pm_over[1]);
        }
        return;
    }
    if (!playing(g)) {
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        aos_hal_beep(700, 30);
        pause_show(a);
        return;
    }

    int v = 0;      /* px a step, signed */
    if (p->x <= -PAD_DEAD || p->x >= PAD_DEAD) {
        v = p->x * PAD_STICK_MAX / 32767;
        a->gp_run = 0;
    } else {
        int dir = aos_pad_held(p, AOS_PAD_RIGHT) - aos_pad_held(p, AOS_PAD_LEFT);
        if (dir && a->gp_run < 255) a->gp_run++;
        if (!dir) a->gp_run = 0;
        int sp = PAD_DPAD_MIN + a->gp_run;
        v = dir * (sp < PAD_DPAD_MAX ? sp : PAD_DPAD_MAX);
    }
    /* a finger on the canvas has the last word */
    if (v && !a->finger) {
        g->touching = 1;
        g->touch_x = (int16_t)(g->pad_x + v * FX_ONE);
    }
    if (aos_pad_pressed(p, AOS_PAD_A)) {
        g->fire_edge = 1;       /* serves; with the laser, also a shot */
    }
    if (aos_pad_held(p, AOS_PAD_A)) {
        g->fire_down = 1;
    }
}

static void step(void *user)
{
    app_t *a = (app_t *)user;
    ak_t *g = &a->g;

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this call 'a' no longer
         * exists, so nothing else is touched (the service sees it too) */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    read_input(a);
    pad_step(a);

    if (g->state == ST_PAUSE || g->state == ST_TITLE ||
        g->state == ST_OVER || g->state == ST_WIN) {
        if ((g->state == ST_OVER || g->state == ST_WIN) && !a->over_shown) {
            show_end(a, g->state == ST_WIN);
        }
        return;                 /* with a panel on top there is nothing to animate */
    }

    ak_step(g);
    /* drawn here, per step and not per draw: the dirty lists are per step */
    present(a);

    if ((g->state == ST_OVER || g->state == ST_WIN) && !a->over_shown) {
        show_end(a, g->state == ST_WIN);
    }
}

static void draw(void *user)
{
    app_t *a = (app_t *)user;
    aos_retro_stats_t st;
    aos_retro_stats(&st);
    a->g.fps10 = (int16_t)st.fps10;
}

/* --------------------------------------------------------------------------
 * Input the runtime delivers
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    if (playing(&a->g)) {
        pause_show(a);
        return true;        /* the pause panel already has its own exit button */
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    ak_t *g = &a->g;
    int32_t v = 0;

    if (aos_hal_pref_get_i32(KEY_HI, &v) && v > 0) {
        g->hiscore = (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
        s_sfx = (v != 0);
    }
    if (aos_hal_pref_get_i32(KEY_FPS, &v)) {
        g->show_fps = (uint8_t)(v ? 1 : 0);
    }
}

/* The canvas for the root's shape, the stage over it and the panels. Called
 * on opening and again when the screen turns. */
static bool layout(app_t *a, lv_obj_t *root)
{
    lv_obj_update_layout(root);
    ak_geo_set(lv_obj_get_width(root) > lv_obj_get_height(root));

    /* only the finger on the canvas: the score strip, the deck and the II
     * button are the game's own drawing, so the OS draws no controls and the
     * canvas takes the whole screen (x3) */
    a->r = aos_retro_begin(root, AK_W, AK_H, 0, AOS_RETRO_TOUCH);
    if (!a->r) {
        return false;
    }
    ak_buf_init(&a->g.fb, a->r->px, AK_W, AK_H);
    ak_buf_init(&a->g.bg, a->bgmem, AK_W, AK_H);

    /* the panels live on a stage exactly over the canvas */
    a->stage = lv_obj_create(root);
    lv_obj_remove_style_all(a->stage);
    lv_obj_set_pos(a->stage, a->r->x, a->r->y);
    lv_obj_set_size(a->stage, a->r->w * a->r->scale, a->r->h * a->r->scale);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_CLICKABLE);

    build_title(a, a->stage);
    build_pause(a, a->stage);
    build_over(a, a->stage);
    return true;
}

static void *arkanos_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }

    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("arkanos", "opening | internal %u B, psram %u B",
                (unsigned)heap_int, (unsigned)heap_psram);

    /* The frame is the OS's canvas; the background with the bricks is ours
     * (malloc sends it to PSRAM): it is the price of the dirty rectangles.
     * Portrait and landscape have the same pixels, so it serves both. */
    a->bgmem = (uint16_t *)malloc((size_t)AK_PX_MAX * sizeof(uint16_t));
    if (!a->bgmem || !layout(a, root)) {
        aos_hal_log("arkanos", "out of memory for the canvas");
        aos_retro_end();
        free(a->bgmem);
        lv_free(a);
        return NULL;
    }
    memset(a->bgmem, 0, (size_t)AK_PX_MAX * sizeof(uint16_t));
    a->g.rng = (uint32_t)aos_hal_uptime_ms() | 1u;

    prefs_load(a);

    a->g.state = ST_TITLE;
    a->g.level = 0;
    ak_bg_build(&a->g);         /* the menu lets the first wall show through behind */
    ak_draw_hud(&a->g);
    push_all(a);
    ak_dirty_reset(&a->g.d_bg);
    title_refresh(a);
    overlay_show(a, a->title);

#ifdef AOS_SIM_BUILTIN
    /* Shortcuts for designing without playing for twenty minutes. They only
     * exist in the simulator: on the board there are no environment variables.
     *
     *   ARK_LEVEL=8  starts on that level
     *   ARK_AUTO=1   the paddle plays itself, ideal for leaving it running
     *   ARK_LIVES=1  to reach the ending panel quickly
     *   ARK_FPS=1    shows frames per second and % of the canvas pushed
     */
    if (getenv("ARK_FPS")) {
        a->g.show_fps = 1;
    }
    const char *env_level = getenv("ARK_LEVEL");
    if (env_level || getenv("ARK_AUTO")) {
        a->g.autoplay = getenv("ARK_AUTO") ? 1 : 0;
        overlay_hide_all(a);
        ak_game_start(&a->g);
        if (env_level) {
            ak_load_level(&a->g, atoi(env_level));
        }
        if (getenv("ARK_LIVES")) {
            a->g.lives = (int8_t)atoi(getenv("ARK_LIVES"));
            a->g.hud_dirty = 1;
        }
    }
#endif

    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("arkanos", "ready | internal %u B, psram %u B | canvas %dx%d x%d",
                (unsigned)heap_int, (unsigned)heap_psram, AK_W, AK_H, a->r->scale);

    aos_retro_run(1000 / AK_FRAME_MS, step, draw, a);
    return a;
}

/* The screen turned. The game goes on where it was: the canvas and the panels
 * are made again for the new shape, what moves is carried over to the new
 * field (ak_relayout), and a rally in progress waits under the pause. */
static bool arkanos_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    lv_obj_update_layout(root);
    bool land = lv_obj_get_width(root) > lv_obj_get_height(root);
    if (a->r && land == (ak_geo.land != 0)) {
        return true;            /* the same shape: nothing to redo */
    }

    ak_geo_t old = ak_geo;
    aos_pad_menu_set(&a->menu, NULL, 0, 0);     /* its buttons are about to go */
    if (a->stage) {
        lv_obj_delete(a->stage);        /* the panels go with it */
    }
    a->stage = NULL;
    a->title = a->pause = a->over = NULL;
    a->chip_sfx = a->chip_fps = a->chip_sfx2 = a->chip_fps2 = NULL;
    aos_retro_end();
    a->r = NULL;
    if (!layout(a, root)) {
        return false;           /* the runtime makes the app again */
    }

    ak_relayout(&a->g, &old);
    ak_draw_hud(&a->g);
    push_all(a);
    ak_dirty_reset(&a->g.d_bg);

    if (a->g.state == ST_TITLE) {
        title_refresh(a);
        overlay_show(a, a->title);
    } else if (a->g.state == ST_OVER || a->g.state == ST_WIN) {
        show_end(a, a->g.state == ST_WIN);
    } else if (a->g.state == ST_PAUSE) {
        chips_refresh(a);
        overlay_show(a, a->pause);
    } else {
        pause_show(a);
    }
    aos_retro_run(1000 / AK_FRAME_MS, step, draw, a);
    aos_hal_log("arkanos", "turned | canvas %dx%d x%d", AK_W, AK_H, a->r->scale);
    return true;
}

static void arkanos_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    aos_hal_pref_set_i32(KEY_HI, (int32_t)a->g.hiscore);
    aos_retro_end();
    free(a->bgmem);
    lv_free(a);
}

/* Leaving mid-bounce should not cost a ball: it pauses. */
static void arkanos_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        pause_show(a);
    }
}

static bool arkanos_init(aos_app_t *app)
{
    app->desc.id      = "demo.arkanos";
    app->desc.name    = "ARKANOS";
    app->desc.icon    = LV_SYMBOL_STOP;
    app->desc.icon_vec = AOS_ICON_BRICKS;
    app->desc.color_a = 0xFF9F0A;
    app->desc.color_b = 0x0A84FF;
    app->desc.order   = 147;
    /* LONG_DRAG: the paddle is a drag across the whole field, much longer
     * than LVGL's 50 px gesture limit, and it must not lose its release (the
     * release is what launches the ball). No orientation flag: portrait is
     * the better field, but it plays in landscape too (resize()). */
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                        AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = arkanos_create;
    app->destroy = arkanos_destroy;
    app->hide    = arkanos_hide;
    app->back    = app_back;
    app->resize  = arkanos_resize;
    return true;
}

AOS_APP_ENTRY(arkanos_init);
