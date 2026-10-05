/*
 * CLAUDE JUMP - the app
 *
 * The only thing that sees LVGL, the HAL and the preferences. The game itself
 * does not know any of the three exist (see cjump.h).
 *
 * Living here:
 *   - the flush to the screen by dirty rectangles (present())
 *   - the four LVGL screens: menu, shop, pause and game over
 *   - the costume shop and its magnified preview
 *   - the controls: the OS's two side buttons and pause, and the finger
 *     (a USB gamepad presses those buttons, and goes through the screens)
 *   - the preferences: record, coins, costume worn and which are bought
 *   - turning the screen (cjump_resize)
 *
 * The canvas is the OS's retro canvas (aos_retro.h) at x3, and it fills the
 * screen: 240x426 = 720x1278 standing up, with the two buttons floating over
 * its bottom corners, and 240x240 = 720x720 lying down, centred, with one
 * button in each black column beside it. The OS scales only the rectangles
 * presented, by hardware on the board. Holding a button walks the critter
 * that way; the finger on the canvas takes it straight to where it is. The
 * pace is the OS's fixed tick.
 *
 * It is the system's first app born multilingual: every visible string comes
 * wrapped in _() from the first commit, instead of being written in Spanish
 * and wrapped afterwards. And the only thing drawn on the canvas is numbers,
 * precisely so the 5x7 font -which has no accents- never touches a translated
 * string.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_retro.h"
#include "aos_pad_menu.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"

#include "cjump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Preferences. A prefix of its own, as the house rules require.
 * -------------------------------------------------------------------------- */
#define KEY_HI      "cj_hi"
#define KEY_COINS   "cj_coins"
#define KEY_SKIN    "cj_skin"
#define KEY_OWNED   "cj_own"
#define KEY_SFX     "cj_sfx"
#define KEY_FPS     "cj_fps"

#define FPS             30          /* steps a second, the game's pace       */

/* The screens are laid out on a 368x448 design grid (the size they were first
 * drawn at). Sizes go x1.5; positions go through X() and Y(), which centre the
 * grid on the stage and, standing up, spread it down the tall screen instead
 * of leaving it in a 672 px band in the middle. The spread moves things apart
 * and keeps their size: a button 2.4 times taller would be a wall. */
#define S(v)            ((v) * 3 / 2)

static struct {
    int  w, h;                      /* the stage: the canvas on the screen    */
    int  ox, oy;                    /* where the design grid's 0,0 lands      */
    int  ky;                        /* vertical spread, in hundredths         */
    bool tall;                      /* portrait                               */
} L;

static inline int X(int v) { return L.ox + S(v); }
static inline int Y(int v) { return L.oy + v * L.ky / 100; }

/* Shop preview: the critter in a small buffer upscaled x8 standing up and x6
 * lying down. It is its own LVGL canvas, not the retro canvas. */
#define PV_W            36
#define PV_H            36
#define PV_SCALE_MAX    8

/* Coin bonus on finishing: one every 20 metres. A collected coin is worth 2
 * and stomping a critter is worth 5, so going out to collect them pays far
 * better than climbing straight up: the coin has to reward risk, not playing
 * time. The costume prices are calibrated against this (cj_skins.c). */
#define BONUS_PER_M     20

typedef struct {
    cj_t       g;

    lv_obj_t  *root;
    lv_obj_t  *stage;               /* the screens' parent, over the canvas    */
    const aos_retro_t *r;
    uint16_t  *bgmem;

    /* preview (menu and shop) */
    lv_obj_t  *preview;
    uint16_t  *pvmem, *pvbig;
    cj_buf_t   pvbuf;
    int        pv_scale;

    /* The two side buttons are the OS's (AOS_RETRO_LR_SPLIT, drawn and
     * read by the retro service with both fingers); what is left here is
     * which one wins when both are held. */
    uint8_t    last_dir;            /* the one pressed last: it wins a tie     */

    /* screens */
    lv_obj_t  *title, *shop, *pause, *over;
    lv_obj_t  *lbl_best, *lbl_coins, *lbl_wallet;
    lv_obj_t  *chip_sfx, *lbl_ayuda;
    lv_obj_t  *lbl_skin, *lbl_price, *btn_action, *lbl_action;
    lv_obj_t  *lbl_over_t, *lbl_over_s, *lbl_over_c, *lbl_over_b;

    /* app state */
    uint32_t   coins_total;
    uint32_t   owned;               /* bitmap of bought costumes               */
    int8_t     shop_idx;
    bool       want_exit;
    bool       over_shown;
    bool       closing;             /* see cjump_destroy()                     */
    uint32_t   last_gesture_ms;

    /* A USB gamepad: in play it presses the OS's buttons by itself; here
     * it goes through the screens (see "The gamepad") */
    aos_pad_t  gp;
    aos_pad_menu_t menu;
    lv_obj_t  *menu_panel;          /* the screen the menu went through last step */
    lv_obj_t  *pm_title[3], *pm_shop[4], *pm_pause[3], *pm_over[2];

    uint8_t    auto_wait;           /* CJ_AUTO: frames before retrying         */
    uint8_t    arrows;              /* the buttons held on the previous step   */
    uint16_t   frames;
} app_t;

static bool s_sfx = true;

/* --------------------------------------------------------------------------
 * Sound
 *
 * cj_game.c calls it without knowing there is a HAL on the other side.
 * aos_hal_beep() enqueues and plays from its own task, so it does not block
 * the drawing.
 * -------------------------------------------------------------------------- */
void cj_sfx(int freq_hz, int ms)
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
 * Flush to the screen
 *
 * The usual three steps, and none of them touches the whole screen unless it
 * has to. It is identical to arkanos's because the problem is the same.
 * -------------------------------------------------------------------------- */

static void push_rect(app_t *a, const cj_rect_t *r)
{
    (void)a;
    aos_retro_present_rect(r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
}

static void push_all(app_t *a)
{
    aos_retro_present();
    a->g.last_area = 100;
}

static void present(app_t *a)
{
    cj_t *g = &a->g;

    /* Zone change: the background is rebuilt whole and everything is pushed.
     * It is the game's only expensive frame, and it happens every few hundred
     * metres. */
    if (g->zone_changed) {
        g->zone_changed = 0;
        cj_bg_build(g);
        g->hud_dirty = 1;
        /* The new background is already copied over fb: whatever was drawn on
         * top of the old one has to be forgotten or the next frame's restore
         * would erase areas that are already correct. */
        cj_dirty_reset(&g->d_prev);
        for (int i = 0; i < MAX_PLATS; i++) g->plats[i].drawn = 0;
        for (int i = 0; i < MAX_COINS; i++) g->coins[i].drawn = 0;
        for (int i = 0; i < MAX_BUGS;  i++) g->bugs[i].drawn  = 0;
        for (int i = 0; i < MAX_PARTS; i++) g->parts[i].drawn = 0;
        g->hero_drawn = 0;
        cj_draw_movers(g);
        cj_draw_hud(g);
        push_all(a);
        g->d_prev = g->d_cur;
        return;
    }

    /* 1. restore from the background whatever we dirtied last frame */
    g->d_push = g->d_prev;

    if (g->d_push.all) {
        const cj_rect_t entera = { 0, 0, CJ_W, (int16_t)CJ_H };
        cj_restore(g->fb.px, a->bgmem, &entera);
    } else {
        for (int i = 0; i < g->d_push.n; i++) {
            cj_restore(g->fb.px, a->bgmem, &g->d_push.r[i]);
        }
    }

    /* 2. draw what moves; fills d_cur */
    cj_draw_movers(g);

    /* 3. the score, only if a number changed. It is noted in d_push and not in
     *    d_cur: it has to be pushed, not restored next frame. */
    /* The score changes number on nearly every frame while climbing, and
     * repainting it is a full-width rectangle plus its invalid area. It is
     * refreshed by the clock and not per frame, which is the rule that already
     * came out of the Game of Life and the tuner. */
    if (g->hud_dirty && (a->frames & 3) == 0) {
        cj_draw_hud(g);
    }

    /* 4. present the union: the OS scales only that */
    cj_dirty_join(&g->d_push, &g->d_cur);

    if (g->d_push.all) {
        push_all(a);
    } else {
        for (int i = 0; i < g->d_push.n; i++) {
            push_rect(a, &g->d_push.r[i]);
        }
        g->last_area  = (uint16_t)(cj_dirty_area(&g->d_push) * 100 / (CJ_W * CJ_H));
        g->last_rects = g->d_push.n;
    }

    g->d_prev = g->d_cur;
}

/* --------------------------------------------------------------------------
 * Interface pieces
 *
 * The screens to read and touch are built with LVGL objects and not with the
 * 5x7 font: there vector text wins, and it also has accents.
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, L.w, L.h);        /* the whole stage: the dim covers it */
    lv_obj_set_pos(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);      /* so touches do not pass through */
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_y(l, Y(y));
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int w, int h, uint32_t accent,
                             const lv_font_t *font, lv_event_cb_t cb, void *data)
{
    x = X(x); y = Y(y); w = S(w); h = S(h);
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C24), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* The press highlight goes by colour and not by transform_scale: scaling
     * forces LVGL to build a separate layer, and a layer that does not fit in
     * memory is a hang, not a slowdown. */
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    /* Fixed width AND height with an ellipsis: these buttons are at absolute
     * positions and German grows 20%. Ugly but contained beats drawn over the
     * neighbour. One line tall, so centring it centres the text: at the
     * button's height the text sat at the top of the shop's tall arrows. */
    lv_obj_set_size(l, w - 12, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
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

static lv_obj_t *make_chip(lv_obj_t *parent, int x, int y, int w,
                           lv_event_cb_t cb, void *data)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, S(w), S(32));
    lv_obj_set_pos(c, X(x), Y(y));
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &aos_montserrat_20, 0);
    lv_obj_set_size(l, S(w) - 12, S(24));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */

static void prefs_save_all(app_t *a)
{
    aos_hal_pref_set_i32(KEY_HI,    (int32_t)a->g.hiscore);
    aos_hal_pref_set_i32(KEY_COINS, (int32_t)a->coins_total);
    aos_hal_pref_set_i32(KEY_SKIN,  a->g.skin);
    aos_hal_pref_set_i32(KEY_OWNED, (int32_t)a->owned);
}

static void prefs_save_opts(app_t *a)
{
    aos_hal_pref_set_i32(KEY_SFX,  s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_FPS,  a->g.show_fps);
}

static void prefs_load(app_t *a)
{
    int32_t v = 0;

    if (aos_hal_pref_get_i32(KEY_HI, &v) && v > 0) {
        a->g.hiscore = (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_COINS, &v) && v > 0) {
        a->coins_total = (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_OWNED, &v) && v > 0) {
        a->owned = (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_SKIN, &v) && v >= 0 && v < CJ_SKINS) {
        a->g.skin = (uint8_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
        s_sfx = (v != 0);
    }
    if (aos_hal_pref_get_i32(KEY_FPS, &v)) {
        a->g.show_fps = (uint8_t)(v ? 1 : 0);
    }

    /* The first three always come unlocked, even if the preference is empty or
     * was stored by an earlier version. */
    a->owned |= (1u << CJ_SKINS_FREE) - 1u;
    if (!(a->owned & (1u << a->g.skin))) {
        a->g.skin = 0;
    }
}

/* --------------------------------------------------------------------------
 * The critter's preview
 *
 * A 36x36 buffer upscaled x8 (x6 lying down). It is redrawn on changing
 * costume and nothing else: it is not a hot path.
 * -------------------------------------------------------------------------- */

static void preview_draw(app_t *a, int skin)
{
    if (!a->preview) {
        return;
    }
    if (skin < 0 || skin >= CJ_SKINS) {
        skin = 0;
    }

    /* A neutral card with a rule in the costume's colour. Tinting the
     * background with the costume's own colour was tried and is worse: a
     * pastel critter on its own pastel does not stand out, and there are
     * sixteen to compare. */
    cj_fill(&a->pvbuf, cj_rgb(0x0E1018));
    cj_vgrad(&a->pvbuf, 1, 1, PV_W - 2, PV_H - 2,
             cj_rgb(0x2A2E3E), cj_rgb(0x161A26));
    cj_frame(&a->pvbuf, 1, 1, PV_W - 2, PV_H - 2, cj_rgb(cj_skins[skin].body));

    /* The critter goes with the costume's box against the top edge: the hats
     * reach 14 px above the body (see cj_skins.c). */
    cj_hero_draw(&a->pvbuf, (PV_W - HERO_W) / 2, HERO_BOX_H - HERO_H,
                 skin, 0, 0, 0, false);

    cj_expand_n(a->pvmem, PV_W, PV_H, a->pvbig, a->pv_scale);
    lv_obj_invalidate(a->preview);
}

/* --------------------------------------------------------------------------
 * Screens: showing and hiding
 * -------------------------------------------------------------------------- */

static void overlay_hide_all(app_t *a)
{
    lv_obj_t *const panels[] = { a->title, a->shop, a->pause, a->over };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (a->preview) {
        lv_obj_add_flag(a->preview, LV_OBJ_FLAG_HIDDEN);
    }
    /* the pad has no buttons to go through (an empty set also takes the
     * outline off the one it was on) */
    aos_pad_menu_set(&a->menu, NULL, 0, 0);
    aos_retro_show_controls(true);
}

/* There is ONE preview and the menu and the shop share it: it is 162 KB of
 * PSRAM and there is no point having two. Since it is a child of the stage and
 * not of the panel, it has to be brought to the front and put where each
 * screen wants it. */
static void preview_place(app_t *a, int y)
{
    if (!a->preview) {
        return;
    }
    lv_obj_set_pos(a->preview, (L.w - PV_W * a->pv_scale) / 2, Y(y));
    lv_obj_remove_flag(a->preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->preview);
}

/* The buttons are only there while playing: over a menu they would be two
 * big dead buttons. */
static void overlay_show(app_t *a, lv_obj_t *panel)
{
    overlay_hide_all(a);
    if (panel) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        /* and the pad goes through its buttons, from the first one */
        if (panel == a->title) {
            aos_pad_menu_set(&a->menu, a->pm_title, 3, 0);
        } else if (panel == a->shop) {
            aos_pad_menu_set(&a->menu, a->pm_shop, 4, 3);
        } else if (panel == a->pause) {
            aos_pad_menu_set(&a->menu, a->pm_pause, 3, 0);
        } else if (panel == a->over) {
            aos_pad_menu_set(&a->menu, a->pm_over, 2, 0);
        }
    }
    aos_retro_show_controls(panel == NULL);
}

/* --------------------------------------------------------------------------
 * Menu
 * -------------------------------------------------------------------------- */

static void chips_refresh(app_t *a)
{
    chip_set(a->chip_sfx, _("SONIDO"), s_sfx);

    if (a->lbl_ayuda) {
        lv_label_set_text(a->lbl_ayuda,
                          _("Botones a los costados, o arrastrá el dedo"));
    }
}

static void title_refresh(app_t *a)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%s  %u m", _("Récord"), (unsigned)a->g.hiscore);
    lv_label_set_text(a->lbl_best, buf);

    snprintf(buf, sizeof(buf), "%s  %u", _("Monedas"), (unsigned)a->coins_total);
    lv_label_set_text(a->lbl_coins, buf);

    chips_refresh(a);
    preview_draw(a, a->g.skin);
    preview_place(a, 44);
}

/* --------------------------------------------------------------------------
 * Shop
 * -------------------------------------------------------------------------- */

static bool owned(const app_t *a, int i)
{
    return (a->owned & (1u << i)) != 0;
}

static void shop_refresh(app_t *a)
{
    int i = a->shop_idx;
    const cj_skin_t *s = &cj_skins[i];
    char buf[64];

    snprintf(buf, sizeof(buf), "%s  %u", _("Monedas"), (unsigned)a->coins_total);
    lv_label_set_text(a->lbl_wallet, buf);

    snprintf(buf, sizeof(buf), "%s   %d/%d", _(s->name), i + 1, CJ_SKINS);
    lv_label_set_text(a->lbl_skin, buf);

    if (!owned(a, i)) {
        snprintf(buf, sizeof(buf), "%u %s", (unsigned)s->price, _("monedas"));
        lv_label_set_text(a->lbl_price, buf);
        lv_obj_set_style_text_color(a->lbl_price,
                                    lv_color_hex(a->coins_total >= s->price
                                                 ? 0xFFD60A : 0xFF453A), 0);
        lv_label_set_text(a->lbl_action, a->coins_total >= s->price
                                         ? _("Comprar") : _("No alcanza"));
    } else if (a->g.skin == i) {
        /* Worn: there is no action at all, so the button hides itself instead
         * of repeating what the status line already says. A button saying the
         * same thing as the notice above it and doing nothing when touched
         * reads as a screen that does not respond. */
        lv_label_set_text(a->lbl_price, _("Puesto"));
        lv_obj_set_style_text_color(a->lbl_price, lv_color_hex(0x30D158), 0);
    } else {
        lv_label_set_text(a->lbl_price, _("Comprado"));
        lv_obj_set_style_text_color(a->lbl_price, lv_color_hex(0x9AA3B8), 0);
        lv_label_set_text(a->lbl_action, _("Ponerme este"));
    }
    if (owned(a, i) && a->g.skin == i) {
        lv_obj_add_flag(a->btn_action, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(a->btn_action, LV_OBJ_FLAG_HIDDEN);
    }

    preview_draw(a, i);
    preview_place(a, 80);
}

static void shop_move(app_t *a, int delta)
{
    a->shop_idx = (int8_t)((a->shop_idx + delta + CJ_SKINS) % CJ_SKINS);
    cj_sfx(900, 18);
    shop_refresh(a);
}

static void shop_action_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    int i = a->shop_idx;
    const cj_skin_t *s = &cj_skins[i];

    if (!owned(a, i)) {
        if (a->coins_total < s->price) {
            cj_sfx(200, 60);
            aos_ui_toast(_("Todavía no alcanza"), 1200);
            return;
        }
        a->coins_total -= s->price;
        a->owned |= (1u << i);
        cj_sfx(700, 40);
        cj_sfx(1100, 50);
        cj_sfx(1500, 70);
        aos_ui_toast(_("Disfraz desbloqueado"), 1200);
    }
    a->g.skin = (uint8_t)i;
    prefs_save_all(a);
    shop_refresh(a);
    cj_sfx(1300, 30);
}

static void shop_prev_cb(lv_event_t *event)
{
    shop_move((app_t *)lv_event_get_user_data(event), -1);
}

static void shop_next_cb(lv_event_t *event)
{
    shop_move((app_t *)lv_event_get_user_data(event), +1);
}

/* The menu is drawn over the first zone's sky, repainted from scratch: if the
 * game's last frame were left, behind the menu you would see zone 4's sky with
 * its score and its platforms frozen. */
static void title_backdrop(app_t *a)
{
    a->g.zone    = 0;
    a->g.bg_seed = cj_rand(&a->g);
    cj_bg_build(&a->g);
    cj_dirty_reset(&a->g.d_prev);
    cj_dirty_reset(&a->g.d_push);
    push_all(a);
}

static void go_title(app_t *a)
{
    a->g.state = ST_TITLE;
    title_backdrop(a);
    overlay_show(a, a->title);
    title_refresh(a);
}

static void shop_back_cb(lv_event_t *event)
{
    go_title((app_t *)lv_event_get_user_data(event));
}

static void shop_open_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->g.state = ST_SHOP;
    a->shop_idx = (int8_t)a->g.skin;
    overlay_show(a, a->shop);
    shop_refresh(a);
}

/* --------------------------------------------------------------------------
 * A game
 * -------------------------------------------------------------------------- */

static void game_start(app_t *a)
{
    cj_game_reset(&a->g);
    a->over_shown = false;
    overlay_hide_all(a);
    /* The camera starts where it starts, so there is nothing to restore from
     * the previous frame: everything is drawn and everything is pushed, once. */
    a->g.zone_changed = 1;
    present(a);
}

static void start_cb(lv_event_t *event)
{
    game_start((app_t *)lv_event_get_user_data(event));
}

static void pause_show(app_t *a)
{
    if (a->g.state != ST_PLAY) {
        return;
    }
    a->g.state = ST_PAUSE;
    overlay_show(a, a->pause);
}

static void resume_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    overlay_hide_all(a);
    a->g.state = ST_PLAY;
    /* Coming back from a panel that covered the whole screen there is nothing
     * trustworthy in fb: everything is repainted. */
    a->g.zone_changed = 1;
    present(a);
}

static void menu_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    prefs_save_all(a);
    go_title(a);
}

static void exit_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->want_exit = true;        /* deferred: aos_ui_back() destroys the app  */
}

static void sfx_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    s_sfx = !s_sfx;
    prefs_save_opts(a);
    chips_refresh(a);
    cj_sfx(1200, 30);
}

/* --------------------------------------------------------------------------
 * Game over
 * -------------------------------------------------------------------------- */

/* The panel's texts. Apart from show_end() because turning the screen
 * rebuilds the panel with the game already over, and the coins must not be
 * paid twice. */
static void over_fill(app_t *a)
{
    cj_t *g = &a->g;
    char buf[64];
    uint32_t bonus = g->score / BONUS_PER_M;
    uint32_t total = g->coins_run + bonus;

    lv_label_set_text(a->lbl_over_t, g->new_record ? _("Nuevo récord")
                                                   : _("Se acabó"));
    lv_obj_set_style_text_color(a->lbl_over_t,
                                lv_color_hex(g->new_record ? 0xFFD60A : 0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "%u m", (unsigned)g->score);
    lv_label_set_text(a->lbl_over_s, buf);

    snprintf(buf, sizeof(buf), "+%u %s", (unsigned)total, _("monedas"));
    lv_label_set_text(a->lbl_over_c, buf);

    snprintf(buf, sizeof(buf), "%s %u   %s %u",
             _("Juntadas"), (unsigned)g->coins_run,
             _("por altura"), (unsigned)bonus);
    lv_label_set_text(a->lbl_over_b, buf);
}

static void show_end(app_t *a)
{
    cj_t *g = &a->g;

    a->over_shown = true;
    a->coins_total += g->coins_run + g->score / BONUS_PER_M;
    prefs_save_all(a);

    over_fill(a);
    overlay_show(a, a->over);
    cj_sfx(g->new_record ? 1400 : 500, 60);
}

/* --------------------------------------------------------------------------
 * Control
 * -------------------------------------------------------------------------- */

/* The buttons walk it: while one is held the target runs ahead of the
 * critter that way, so it goes at full speed; on letting go the target stays
 * a step ahead, so it glides to a stop instead of halting dead. The finger on
 * the canvas takes it to where the finger is. The same target_x, so the game
 * (cj_game.c) does not know which one it was. */
static void read_controls(app_t *a)
{
    cj_t *g = &a->g;
    uint32_t down = aos_retro_pressed();
    uint32_t btns = aos_retro_buttons();
    const int32_t span = (int32_t)(CJ_W - HERO_W) * FX;

    /* bit 0 left, bit 1 right, as the game's own buttons had them */
    uint8_t held  = (uint8_t)(((btns & AOS_RETRO_BTN_LEFT) ? 1 : 0) | ((btns & AOS_RETRO_BTN_RIGHT) ? 2 : 0));
    uint8_t press = (uint8_t)(((down & AOS_RETRO_BTN_LEFT) ? 1 : 0) | ((down & AOS_RETRO_BTN_RIGHT) ? 2 : 0));
    /* A tap is a finger LANDING on the canvas (not on a button, and not
     * one sliding in from the top edge, the system's swipe): on the score
     * strip it pauses. Read every step, so none is left over for later. */
    int tx, ty;
    bool hud = aos_retro_tap(&tx, &ty) && ty < CJ_HUD_H;
    int cx = 0;
    bool canvas_on = aos_retro_touch(&cx, NULL);

    if (g->state != ST_PLAY) {
        a->arrows = 0;
        g->touching = 0;
        return;
    }
    /* The OS's pause button, or a tap on the score strip. A pad's START
     * presses the OS's pause too, but the pad answers it itself (pad_step()):
     * taken twice, the press that resumes would pause again. */
    if ((down & AOS_RETRO_BTN_PAUSE) &&
        ((a->gp.held | a->gp.released) & AOS_PAD_START)) {
        down &= ~(uint32_t)AOS_RETRO_BTN_PAUSE;
    }
    if ((down & AOS_RETRO_BTN_PAUSE) || hud) {
        pause_show(a);
        return;
    }

    /* A tap shorter than a step went down and up between two of them: it
     * still counts, as one step's worth of holding. With both held, the one
     * pressed last wins, as on a pad: rolling the thumbs from one button to
     * the other must not stop the critter halfway. Both landing in the same
     * sample say nothing, so the critter keeps its course. */
    if (press == 1 || press == 2) {
        a->last_dir = press;
    }
    uint8_t arrows = held | press;
    if (arrows == 3) {
        arrows = a->last_dir;       /* both at once from rest: neither        */
    } else if (!arrows) {
        a->last_dir = 0;
    }

    if (arrows == 1) {
        g->target_x = g->hx - 24 * FX;
    } else if (arrows == 2) {
        g->target_x = g->hx + 24 * FX;
    } else if (a->arrows && !arrows) {
        g->target_x = g->hx + g->hvx;
    }
    a->arrows = arrows;

    bool touching = canvas_on && !arrows;
    if (touching) {
        g->touch_x = (int16_t)cx;
        g->target_x = clampi((int)g->touch_x - HERO_W / 2, 0, CJ_W - HERO_W) * FX;
    }
    g->touching = touching ? 1 : 0;

    if (g->target_x < 0) g->target_x = 0;
    if (g->target_x > span) g->target_x = span;
}

/* --------------------------------------------------------------------------
 * Gestures
 *
 * They arrive by two paths: LV_EVENT_GESTURE on the root (which has to be
 * listened for on the ROOT and with GESTURE_BUBBLE taken off it, because LVGL
 * delivers it to the first ancestor that does not have it) and
 * aos_ui_take_gesture(), which is what the touch driver itself detects when
 * the swipe is fast and LVGL never gathers its 50 px. Since both may arrive,
 * the second is discarded if it comes within 400 ms of the first.
 * -------------------------------------------------------------------------- */

static bool handle_gesture(app_t *a, int dir)
{
    /* While playing, a horizontal swipe is NOT a gesture: it is the control.
     * false is returned so the caller does not consume the touch -calling
     * lv_indev_wait_release() here would cut the drag short at 50 px, which is
     * exactly what AOS_APP_FLAG_LONG_DRAG exists to avoid. */
    if (a->g.state != ST_SHOP && a->g.state != ST_TITLE) {
        return false;
    }

    uint32_t now = lv_tick_get();
    if ((uint32_t)(now - a->last_gesture_ms) < 400) {
        return false;
    }
    a->last_gesture_ms = now;

    if (a->g.state == ST_SHOP) {
        shop_move(a, dir == LV_DIR_LEFT ? +1 : -1);
        return true;
    }
    if (dir == LV_DIR_RIGHT) {
        a->want_exit = true;
        return true;
    }
    return false;
}

static void gesture_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (a->closing || !indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) {
        return;
    }
    if (handle_gesture(a, (int)dir)) {
        lv_indev_wait_release(indev);
    }
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * In play a USB pad needs nothing from here: the OS presses the canvas's
 * buttons with it (the d-pad walks, as the side buttons do). On the screens
 * the d-pad goes through the buttons and A presses them (aos_pad_menu.h);
 * START plays from the menu, pauses and resumes, and plays again at the
 * end; B is back (the shop and the end to the menu, out of the pause); L
 * and R go through the costumes in the shop.
 * -------------------------------------------------------------------------- */

static void pad_click(lv_obj_t *b)
{
    if (b && lv_obj_is_valid(b)) {
        lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    }
}

static void pad_step(app_t *a)
{
    aos_pad_t *p = &a->gp;
    lv_obj_t *panel = a->menu.n ? lv_obj_get_parent(a->menu.item[0]) : NULL;

    aos_pad_update(p, lv_tick_get());
    /* a screen that has just come up does not take the press that brought
     * it (that START is pausing, not resuming) */
    bool fresh = panel != a->menu_panel;
    a->menu_panel = panel;
    /* in the background, or under the app switcher, the pad is not ours */
    if (fresh || !p->connected || !lv_obj_is_visible(a->r->view)) {
        return;
    }
    if (!panel) {
        if (a->g.state == ST_PLAY && aos_pad_pressed(p, AOS_PAD_START)) {
            pause_show(a);
        }
        return;
    }
    if (aos_pad_menu_step(&a->menu, p)) {
        return;
    }
    bool start = aos_pad_pressed(p, AOS_PAD_START);
    bool back = aos_pad_pressed(p, AOS_PAD_B);
    if (panel == a->title) {
        if (start) pad_click(a->pm_title[0]);
    } else if (panel == a->shop) {
        if (back) pad_click(a->pm_shop[0]);
        else if (aos_pad_pressed(p, AOS_PAD_L)) shop_move(a, -1);
        else if (aos_pad_pressed(p, AOS_PAD_R)) shop_move(a, +1);
    } else if (panel == a->pause) {
        if (start || back) pad_click(a->pm_pause[0]);
    } else if (panel == a->over) {
        if (start) pad_click(a->pm_over[0]);
        else if (back) pad_click(a->pm_over[1]);
    }
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

static void step(void *user)
{
    app_t *a = (app_t *)user;
    cj_t *g = &a->g;

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this call 'a' no longer
         * exists, so nothing else is touched (the OS's tick sees it too). */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    int d = aos_ui_take_gesture();
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
        handle_gesture(a, d);
    }

    if (a->frames < 0xFFFF) {
        a->frames++;
    }

    /* CJ_AUTO starts over by itself. Without this, leaving the game running
     * for an hour to hunt for dirty-rectangle trails lasts until the first
     * critter: the bot does not dodge them. */
    if (g->autoplay && g->state == ST_OVER && a->over_shown) {
        if (++a->auto_wait > 60) {
            a->auto_wait = 0;
            game_start(a);
        }
        return;
    }

    pad_step(a);
    read_controls(a);

    if (g->state != ST_PLAY && g->state != ST_DYING) {
        return;                 /* with a panel on top there is nothing to animate */
    }

    cj_step(g);
    present(a);

    if (g->state == ST_OVER && !a->over_shown) {
        show_end(a);
    }

    /* The fps counter is refreshed by the clock and not per frame: writing it
     * on every one invalidates its box ten times a second for nothing. */
    if (g->show_fps && (a->frames & 7) == 0) {
        g->hud_dirty = 1;
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
 * Back
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    switch (a->g.state) {
    case ST_PLAY:
    case ST_DYING:
        pause_show(a);
        return true;
    case ST_SHOP:
    case ST_PAUSE:
    case ST_OVER:
        go_title(a);
        return true;
    default:
        return false;           /* from the menu, let the system leave        */
    }
}

/* --------------------------------------------------------------------------
 * Building the screens
 * -------------------------------------------------------------------------- */

static void build_title(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_panel(root);
    a->title = p;

    make_label(p, "Claude Jump",
               L.tall ? &aos_montserrat_48 : &aos_montserrat_36, 0xFFFFFF, 6);

    /* The record and the coins go on ONE line. They were two and were merged
     * to gain the 26 px needed when the chips moved up. */
    a->lbl_best  = make_label(p, "", &aos_montserrat_28, 0xFFFFFF, 192);
    a->lbl_coins = make_label(p, "", &aos_montserrat_28, 0xFFD60A, 192);
    /* Fixed width and height with an ellipsis: they are two absolute boxes in
     * a row, and German grows 20% over Spanish. */
    lv_obj_set_size(a->lbl_best, S(168), S(26));
    lv_obj_set_x(a->lbl_best, X(8));
    lv_label_set_long_mode(a->lbl_best, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(a->lbl_best, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(a->lbl_coins, S(168), S(26));
    lv_obj_set_x(a->lbl_coins, X(192));
    lv_label_set_long_mode(a->lbl_coins, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(a->lbl_coins, LV_TEXT_ALIGN_LEFT, 0);

    a->pm_title[0] = make_button(p, _("Jugar"), 64, 224, 240, 48, 0x30D158,
                                 &aos_montserrat_36, start_cb, a);
    a->pm_title[1] = make_button(p, _("Tienda"), 64, 278, 240, 38, 0xBF5AF2,
                                 &aos_montserrat_28, shop_open_cb, a);

    a->chip_sfx  = make_chip(p, 128, 322, 112, sfx_cb,  a);
    a->pm_title[2] = a->chip_sfx;

    /* How to play, at the bottom, under everything that is touched. */
    a->lbl_ayuda = make_label(p, "", &aos_montserrat_24, 0x6E7A8C, 372);
}

static void build_shop(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_panel(root);
    a->shop = p;

    /* "Back" at the top left, where every screen of the system has it. */
    a->pm_shop[0] = make_button(p, LV_SYMBOL_LEFT, 8, 8, 66, 36, 0x8E8E93,
                                &aos_montserrat_28, shop_back_cb, a);

    make_label(p, _("Tienda"), &aos_montserrat_36, 0xFFFFFF, 8);
    a->lbl_wallet = make_label(p, "", &aos_montserrat_28, 0xFFD60A, 50);

    /* The arrows go on either side of the preview, which takes the centre,
     * at its middle height in both orientations (its scale differs). */
    a->pm_shop[1] = make_button(p, "<", 4, 116, 44, 76, 0x0A84FF, &aos_montserrat_36,
                                shop_prev_cb, a);
    a->pm_shop[2] = make_button(p, ">", 320, 116, 44, 76, 0x0A84FF, &aos_montserrat_36,
                                shop_next_cb, a);

    a->lbl_skin  = make_label(p, "", &aos_montserrat_36, 0xFFFFFF, 232);
    a->lbl_price = make_label(p, "", &aos_montserrat_28, 0x9AA3B8, 270);

    a->btn_action = make_button(p, "", 64, 304, 240, 46, 0x30D158,
                                &aos_montserrat_28, shop_action_cb, a);
    a->lbl_action = lv_obj_get_child(a->btn_action, 0);
    a->pm_shop[3] = a->btn_action;

    /* Same idea as in the menu: a notice under everything that is touched.
     * Here it teaches the gesture, which otherwise is not discovered. */
    make_label(p, _("Deslizá para ver los demás"), &aos_montserrat_24,
               0x6E7A8C, 374);
}

static void build_pause(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_panel(root);
    a->pause = p;
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);

    make_label(p, _("Pausa"), &aos_montserrat_48, 0xFFFFFF, 116);

    a->pm_pause[0] = make_button(p, _("Seguir"), 64, 178, 240, 50, 0x30D158,
                                 &aos_montserrat_36, resume_cb, a);
    a->pm_pause[1] = make_button(p, _("Menú"), 64, 236, 240, 40, 0x0A84FF,
                                 &aos_montserrat_28, menu_cb, a);
    a->pm_pause[2] = make_button(p, _("Salir"), 64, 284, 240, 40, 0xFF453A,
                                 &aos_montserrat_28, exit_cb, a);
}

static void build_over(app_t *a, lv_obj_t *root)
{
    lv_obj_t *p = make_panel(root);
    a->over = p;
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);

    a->lbl_over_t = make_label(p, "", &aos_montserrat_36, 0xFFFFFF, 62);
    a->lbl_over_s = make_label(p, "", &aos_montserrat_64, 0xFFFFFF, 106);
    a->lbl_over_c = make_label(p, "", &aos_montserrat_36, 0xFFD60A, 174);
    a->lbl_over_b = make_label(p, "", &aos_montserrat_24, 0x9AA3B8, 214);

    a->pm_over[0] = make_button(p, _("Otra vez"), 64, 250, 240, 50, 0x30D158,
                                &aos_montserrat_36, start_cb, a);
    a->pm_over[1] = make_button(p, _("Menú"), 64, 308, 240, 40, 0x0A84FF,
                                &aos_montserrat_28, menu_cb, a);
}

/* --------------------------------------------------------------------------
 * The view: canvas, buttons and screens, for the root's current size
 *
 * Built on opening and again each time the screen turns. What survives a
 * turn is the game (cj_t), the coins and the two buffers that are ours; all
 * that hangs from the root is made anew, because every position depends on
 * the orientation.
 * -------------------------------------------------------------------------- */

static bool view_build(app_t *a)
{
    lv_obj_t *root = a->root;
    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);

    /* x3 on this screen whichever way it stands; as many rows as fit at
     * that scale, up to the buffer's 426. */
    int k = W / CJ_W, kh = H / 200;
    if (kh < k) k = kh;
    if (k < 1)  k = 1;
    int h = H / k;
    if (h > CJ_H_MAX) h = CJ_H_MAX;
    cj_canvas_h = h;

    /* OVERLAY: the canvas takes the whole screen and the controls float
     * over it. LR_SPLIT: one button per thumb, in the bottom corners either
     * side of the pause pill standing up, low in each black column lying
     * down, where pause tops the right one. TOUCH: the finger on the canvas
     * that is on neither button takes the critter to where it is. (Until
     * 2026-09-30 the side buttons were the game's own, drawn and read here;
     * the service now lays them out and reads them the same way.) */
    a->r = aos_retro_begin(root, CJ_W, h, k,
                           AOS_RETRO_LR_SPLIT | AOS_RETRO_PAUSE | AOS_RETRO_OVERLAY | AOS_RETRO_TOUCH);
    if (!a->r) {
        return false;
    }
    cj_buf_init(&a->g.fb, a->r->px, CJ_W, h);
    cj_buf_init(&a->g.bg, a->bgmem, CJ_W, h);

    L.w    = a->r->w * a->r->scale;
    L.h    = a->r->h * a->r->scale;
    L.tall = H >= W;
    L.ky   = L.tall ? 240 : 150;
    L.ox   = (L.w - S(368)) / 2;
    L.oy   = (L.h - 400 * L.ky / 100) / 2;
    a->pv_scale = L.tall ? PV_SCALE_MAX : 6;

    /* The service's controls are made hidden until the game starts (the
     * menu is up first): they sit under the stage, so the menus and their
     * dim go over them. */
    aos_retro_show_controls(false);

    /* the screens live on a stage exactly over the canvas */
    a->stage = lv_obj_create(root);
    lv_obj_remove_style_all(a->stage);
    lv_obj_set_pos(a->stage, a->r->x, a->r->y);
    lv_obj_set_size(a->stage, L.w, L.h);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_CLICKABLE);

    /* The preview. It is a child of the stage and not of a panel because the
     * menu and the shop share it; preview_place() brings it forward and
     * positions it. */
    int pv = PV_W * a->pv_scale;
    a->preview = lv_canvas_create(a->stage);
    lv_canvas_set_buffer(a->preview, a->pvbig, pv, pv, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->preview, pv, pv);
    lv_image_set_antialias(a->preview, false);
    lv_obj_remove_flag(a->preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->preview, LV_OBJ_FLAG_HIDDEN);

    build_title(a, a->stage);
    build_shop(a, a->stage);
    build_pause(a, a->stage);
    build_over(a, a->stage);

    /* (the service reads touches from begin() on: the finger that opened
     * the app, or the one that turned the screen, is no tap) */

    aos_retro_run(FPS, step, draw, a);
    return true;
}

/* Everything hanging from the root goes; the root and its gesture handler
 * stay. 'closing' keeps the events LVGL sends while deleting away from the
 * handlers, as in cjump_destroy(). */
static void view_drop(app_t *a)
{
    aos_retro_stop();
    aos_pad_menu_set(&a->menu, NULL, 0, 0);     /* its buttons are about to go */
    a->menu_panel = NULL;
    a->closing = true;
    aos_retro_end();
    if (a->root) {
        lv_obj_clean(a->root);
    }
    a->closing = false;
    a->r = NULL;
    a->stage = a->preview = NULL;
    a->title = a->shop = a->pause = a->over = NULL;
    a->chip_sfx = a->lbl_ayuda = NULL;
}

/* After a turn: the same screen as before, over a canvas drawn again. A game
 * in play comes back paused, so the new layout is seen before it goes on. */
static void view_restore(app_t *a, int old_h)
{
    cj_t *g = &a->g;
    switch (g->state) {
    case ST_TITLE:
        go_title(a);
        break;
    case ST_SHOP:
        title_backdrop(a);
        overlay_show(a, a->shop);
        shop_refresh(a);
        break;
    default:
        cj_game_refit(g, old_h);
        g->zone_changed = 1;
        present(a);
        if (g->state == ST_PLAY) {
            pause_show(a);
        } else if (g->state == ST_PAUSE) {
            overlay_show(a, a->pause);
        } else if (g->state == ST_OVER) {
            over_fill(a);
            overlay_show(a, a->over);
        } else {
            overlay_hide_all(a);            /* dying: let it fall          */
        }
        break;
    }
}

static bool cjump_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || root != a->root) {
        return false;
    }
    int old_h = CJ_H;
    view_drop(a);
    if (!view_build(a)) {
        return false;           /* the runtime destroys and creates it again */
    }
    view_restore(a, old_h);
    aos_hal_log("cjump", "turned | canvas %dx%d x%d", CJ_W, CJ_H, a->r->scale);
    return true;
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void *cjump_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }

    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("cjump", "opening | internal %u B, psram %u B",
                (unsigned)heap_int, (unsigned)heap_psram);

    /* The frame is the OS's canvas; the background is ours and so is the
     * preview (200 + 2 + 162 KB), all through malloc(), which sends them to
     * PSRAM. Both at their biggest, so a turn of the screen reuses them. */
    size_t chico = (size_t)CJ_W * CJ_H_MAX * sizeof(uint16_t);
    size_t big   = (size_t)PV_W * PV_H * PV_SCALE_MAX * PV_SCALE_MAX *
                   sizeof(uint16_t);
    a->bgmem = (uint16_t *)malloc(chico);
    a->pvmem = (uint16_t *)malloc((size_t)PV_W * PV_H * sizeof(uint16_t));
    a->pvbig = (uint16_t *)malloc(big);
    if (!a->bgmem || !a->pvmem || !a->pvbig) {
        aos_hal_log("cjump", "out of memory for the buffers");
        free(a->bgmem);
        free(a->pvmem);
        free(a->pvbig);
        lv_free(a);
        return NULL;
    }
    memset(a->bgmem, 0, chico);
    memset(a->pvmem, 0, (size_t)PV_W * PV_H * sizeof(uint16_t));
    memset(a->pvbig, 0, big);

    cj_buf_init(&a->pvbuf, a->pvmem, PV_W, PV_H);
    a->g.rng = (uint32_t)aos_hal_uptime_ms() | 1u;
    a->owned = (1u << CJ_SKINS_FREE) - 1u;

    prefs_load(a);

    a->root = root;
    if (!view_build(a)) {
        aos_hal_log("cjump", "out of memory for the canvas");
        free(a->bgmem);
        free(a->pvmem);
        free(a->pvbig);
        lv_free(a);
        return NULL;
    }

    /* The gesture is listened for on the ROOT and with GESTURE_BUBBLE taken
     * off it: LVGL does not send LV_EVENT_GESTURE to the object under the
     * finger, it climbs through the parents as long as it finds the flag and
     * gives it to the first one that does not have it. A transparent layer at
     * the bottom of the z-order is never on that path. */
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, a);

    /* The menu lets the first zone's sky show through behind. */
    go_title(a);

#ifdef AOS_SIM_BUILTIN
    /* Development switches. They only exist in the simulator: on the board
     * getenv() always returns NULL.
     *
     *   CJ_AUTO=1     the critter plays itself, for leaving it running and
     *                 hunting trails from badly recorded dirty rectangles
     *   CJ_FPS=1      frames per second and % of screen pushed
     *   CJ_COINS=500  coins, for testing the shop without playing
     *   CJ_SKIN=11    costume worn
     *   CJ_OWN=1      every costume unlocked
     *   CJ_SHOP=1     opens straight into the shop
     *   CJ_ZONE=3     starts the game in that zone
     *   CJ_SCREEN=pause|over   opens straight into that panel
     *
     * CJ_SCREEN exists for the layout audit: audit_layout.sh opens each app
     * with AOS_SIM_VIEW and audits what it sees, and what it sees is the menu
     * -the other three panels are born hidden and the auditor skips what is
     * hidden-. With this they can be audited one at a time:
     *
     *   AOS_SIM_AUDIT=de/cjump.over CJ_SCREEN=over AOS_SIM_VIEW=demo.cjump ...
     */
    const char *env;
    if ((env = getenv("CJ_FPS")) && env[0]) {
        a->g.show_fps = 1;
    }
    if ((env = getenv("CJ_COINS")) && env[0]) {
        a->coins_total = (uint32_t)atoi(env);
    }
    if ((env = getenv("CJ_OWN")) && env[0]) {
        a->owned = 0xFFFFu;
    }
    if ((env = getenv("CJ_SKIN")) && env[0]) {
        int v = atoi(env);
        if (v >= 0 && v < CJ_SKINS) {
            a->g.skin = (uint8_t)v;
            a->owned |= (1u << v);
        }
    }
    if ((env = getenv("CJ_SHOP")) && env[0]) {
        a->shop_idx = (int8_t)a->g.skin;
        a->g.state = ST_SHOP;
        overlay_show(a, a->shop);
        shop_refresh(a);
    } else if ((env = getenv("CJ_AUTO")) && env[0]) {
        a->g.autoplay = 1;
        game_start(a);
    }
    if ((env = getenv("CJ_SCREEN")) && env[0]) {
        if (env[0] == 'p') {                    /* pause */
            game_start(a);
            pause_show(a);
        } else if (env[0] == 'o') {             /* over */
            game_start(a);
            a->g.score      = 1234;
            a->g.coins_run  = 27;
            a->g.new_record = 1;
            a->g.state      = ST_OVER;
            show_end(a);
        }
    }
    if ((env = getenv("CJ_ZONE")) && env[0]) {
        int v = atoi(env);
        if (v > 0 && v < CJ_ZONES && a->g.state == ST_PLAY) {
            /* The height is faked: the zone comes from the metres, so moving
             * the origin is the only way to get into a real zone. */
            a->g.start_y += (int32_t)cj_zones[v].from_m * 4 * FX;
            a->g.score = cj_zones[v].from_m;
            a->g.zone  = (uint8_t)v;
            a->g.zone_changed = 1;
        }
    }
    /* Only if the switches have not already started on another screen: without
     * this guard, title_refresh() shows the menu's preview again on top of the
     * game CJ_AUTO has just started. */
    if (a->g.state == ST_TITLE) {
        title_refresh(a);
    }
#endif

    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("cjump", "ready | internal %u B, psram %u B | canvas %dx%d x%d",
                (unsigned)heap_int, (unsigned)heap_psram, CJ_W, CJ_H, a->r->scale);
    return a;
}

static void cjump_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    aos_retro_stop();

    /* The objects are deleted HERE and not left to the runtime. close_current()
     * calls destroy() and only then deletes the root, so everything LVGL sends
     * on deleting -LV_EVENT_DELETE, and LV_EVENT_PRESS_LOST if a finger was
     * down- would reach callbacks with the context already freed. With the
     * flag set and the deletion done here, those last events are harmless and
     * the empty root the runtime inherits it deletes all the same. */
    a->closing = true;
    if (a->root) {
        lv_obj_clean(a->root);
    }

    /* The coins of a half-played game are not lost: if you leave with the
     * gesture mid-flight, what was collected up to then already counts. */
    if (a->g.state == ST_PLAY || a->g.state == ST_DYING ||
        a->g.state == ST_PAUSE) {
        a->coins_total += a->g.coins_run;
        if (a->g.score > a->g.hiscore) {
            a->g.hiscore = a->g.score;
        }
    }
    prefs_save_all(a);
    prefs_save_opts(a);

    aos_hal_log("cjump", "closing | best %u m, %u coins, costume %d",
                (unsigned)a->g.hiscore, (unsigned)a->coins_total, a->g.skin);

    aos_retro_end();
    free(a->bgmem);
    free(a->pvmem);
    free(a->pvbig);
    lv_free(a);
}

/* Leaving mid-jump should not cost the game: it pauses. */
static void cjump_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        pause_show(a);
    }
}

static bool cjump_init(aos_app_t *app)
{
    app->desc.id      = "demo.cjump";
    app->desc.name    = "Claude Jump";
    app->desc.icon    = LV_SYMBOL_UP;
    app->desc.icon_vec = AOS_ICON_JUMP;
    /* Sky over grass, which is the game's first zone. The gradient has to be
     * darkish at both ends: the icon catalogue draws the shapes in white, so
     * the colour is set by the app. The first attempt was Claude orange over
     * green and the middle came out brown. */
    app->desc.color_a = 0x4A9DF5;
    app->desc.color_b = 0x2E9E52;
    app->desc.order   = 141;        /* next to Claudito, who is the same critter */
    /* No orientation flag: it plays both ways (portrait is where it is at
     * home, landscape letterboxes it between the two buttons) and follows
     * the system's setting. */
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                        AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = cjump_create;
    app->destroy = cjump_destroy;
    app->hide    = cjump_hide;
    app->resize  = cjump_resize;
    app->back    = app_back;
    return true;
}

AOS_APP_ENTRY(cjump_init);
