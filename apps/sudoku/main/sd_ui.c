/*
 * SUDOKU - the screens.
 *
 * The root holds, bottom to top: the theme's picture (an lv_image of the
 * 1280 x 1280 picture, centred, so each orientation shows its middle), the
 * moving sprites, and the screen of the moment, built from scratch on every
 * change of screen or orientation. The app is full screen and keeps its
 * buttons out of the system's edge strips (the top pulls the notifications,
 * the bottom goes home), like Gemas.
 *
 *   menu      the game in progress, the daily challenge, a new game by
 *             variant and level, and the shop, statistics and settings
 *   game      the board, the tools (undo, redo, erase, notes, automatic
 *             notes, hint) and the number pad, with its panels: the hint,
 *             the pause (which covers the board), waiting for a board and
 *             the end with what it paid
 *   shop      the themes with their thumbnails and prices
 *   preview   a theme on a demo board, to buy it or put it on
 *   stats     by mode and level: played, won, best and average time
 *   settings  showing errors, highlights, notes, sound, the moving background
 */
#include "sd_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sd_ui_t sd_U;

#define TOP_P   56
#define BOT_P   40
#define TOP_L   44
#define BOT_L   30
#define SIDE    18

static const char *const DIGITS[10] = { "", "1", "2", "3", "4", "5", "6", "7", "8", "9" };

enum { PANEL_NONE = 0, PANEL_HINT, PANEL_PAUSE, PANEL_WAIT, PANEL_WON };

/* ---- helpers ------------------------------------------------------------------------- */

lv_obj_t *sd_box(lv_obj_t *parent, uint32_t color, uint8_t opa, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t *sd_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void sd_fmt_time(char *out, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void sd_ui_toast(const char *msg)
{
    aos_ui_toast(msg, 2200);
}

static uint32_t mix(uint32_t a, uint32_t b, int t)
{
    int r = (int)((a >> 16) & 255) * (255 - t) / 255 + (int)((b >> 16) & 255) * t / 255;
    int g = (int)((a >> 8) & 255) * (255 - t) / 255 + (int)((b >> 8) & 255) * t / 255;
    int bl = (int)(a & 255) * (255 - t) / 255 + (int)(b & 255) * t / 255;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

/* text on the accent: dark on a light accent (a pale green, a sand), else white */
static uint32_t on_accent(const sd_theme_t *t)
{
    uint32_t c = t->accent;
    int y = (int)((c >> 16) & 255) * 299 + (int)((c >> 8) & 255) * 587 + (int)(c & 255) * 114;
    return y > 150000 ? 0x14141A : 0xFFFFFF;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    const sd_theme_t *t = sd_theme();
    lv_obj_t *c = sd_box(parent, t->card, t->card_opa, AOS_UI_RADIUS);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    return c;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, uint8_t opa,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = sd_box(parent, color, opa, h >= 80 ? 24 : 18);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(mix(color, 0xFFFFFF, 60)), LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *menu_items[AOS_PAD_MENU_MAX];
static int menu_n;

static void menu_add(lv_obj_t *o)
{
    if (menu_n < AOS_PAD_MENU_MAX) menu_items[menu_n++] = o;
}

static void menu_commit(int sel)
{
    aos_pad_menu_set(&sd_U.menu, menu_items, menu_n, sel);
}

/* ---- small drawn glyphs: undo, redo, the coin -------------------------------------- */

enum { GL_UNDO = 1, GL_REDO, GL_COIN };

static void glyph_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int kind = (int)(intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = lv_area_get_width(&a), cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_color_t col = lv_obj_get_style_text_color(o, 0);
    if (kind == GL_COIN) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE;
        d.bg_color = lv_color_hex(0xF5B301);
        d.border_color = lv_color_hex(0xB97A00);
        d.border_width = w / 10 + 1;
        d.border_opa = 255;
        lv_draw_rect(layer, &d, &a);
        lv_area_t in = { a.x1 + w / 4, a.y1 + w / 4, a.x2 - w / 4, a.y2 - w / 4 };
        d.bg_color = lv_color_hex(0xFFD54A);
        d.border_width = w / 14 + 1;
        d.border_color = lv_color_hex(0xD99A00);
        lv_draw_rect(layer, &d, &in);
        return;
    }
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = col;
    ad.width = w / 9 + 1;
    ad.center.x = cx;
    ad.center.y = cy + w / 12;
    ad.radius = (uint16_t)(w * 3 / 10);
    ad.rounded = 1;
    bool undo = kind == GL_UNDO;
    ad.start_angle = undo ? 200 : 200;
    ad.end_angle = undo ? 20 : 340;
    if (undo) {
        ad.start_angle = 200;
        ad.end_angle = 30;
    } else {
        ad.start_angle = 150;
        ad.end_angle = 340;
    }
    lv_draw_arc(layer, &ad);
    /* the arrowhead at the arc's left (undo) or right (redo) end */
    int r = ad.radius;
    int hx = undo ? cx - r : cx + r, hy = ad.center.y - 2;
    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    td.color = col;
    td.opa = 255;
    int s = w / 5;
    td.p[0].x = hx - s;
    td.p[0].y = hy - s / 3;
    td.p[1].x = hx + s;
    td.p[1].y = hy - s / 3;
    td.p[2].x = hx;
    td.p[2].y = hy + s;
    lv_draw_triangle(layer, &td);
}

static lv_obj_t *glyph(lv_obj_t *parent, int kind, int size, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_add_event_cb(o, glyph_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)kind);
    return o;
}

/* the coins, top right of a screen */
static void coins_pill(lv_obj_t *parent, int right, int y)
{
    const sd_theme_t *t = sd_theme();
    char buf[24];
    snprintf(buf, sizeof buf, "%u", (unsigned)sd_D.coins);
    lv_obj_t *p = sd_box(parent, t->card, t->card_opa, 30);
    int w = 70 + (int)strlen(buf) * 22;
    lv_obj_set_size(p, w, 60);
    lv_obj_set_pos(p, right - w, y);
    lv_obj_t *g = glyph(p, GL_COIN, 36, 0);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 14, 0);
    sd_U.lbl_coins = sd_label(p, buf, aos_font_body, t->text);
    lv_obj_align(sd_U.lbl_coins, LV_ALIGN_LEFT_MID, 60, 0);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    sd_ui_back();
}

/* a screen's header: back, title, and the coins at the right */
static void header(lv_obj_t *parent, int y, const char *title, bool coins)
{
    const sd_theme_t *t = sd_theme();
    lv_obj_t *b = button(parent, SIDE, y, 64, 64, t->card, t->card_opa, back_cb, NULL);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_center(sd_label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, t->text));
    menu_add(b);
    lv_obj_t *l = sd_label(parent, title, aos_font_title, t->text);
    lv_obj_set_pos(l, SIDE + 84, y + 10);
    if (coins) coins_pill(parent, sd_U.W - SIDE, y + 2);
}

/* ======================================================================================
 * The menu
 * ====================================================================================== */

static void menu_resume_cb(lv_event_t *e)
{
    (void)e;
    sd_snd_play(SD_S_TAP);
    sd_ui_go(SD_SCR_GAME);
}

static void menu_daily_cb(lv_event_t *e)
{
    (void)e;
    uint32_t ymd;
    int wday;
    if (!sd_today(&ymd, &wday)) {
        sd_ui_toast(_("Todavía no hay hora: el desafío del día espera al reloj de la red"));
        return;
    }
    int m, l;
    sd_daily_pick(ymd, wday, &m, &l);
    sd_snd_play(SD_S_TAP);
    sd_ui_start(SD_M_DAILY, l, ymd);
}

static void menu_variant_cb(lv_event_t *e)
{
    sd_D.menu_variant = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    sd_data_save();
    sd_snd_play(SD_S_TAP);
    sd_ui_build();
}

static void menu_level_cb(lv_event_t *e)
{
    int l = (int)(intptr_t)lv_event_get_user_data(e);
    sd_snd_play(SD_S_TAP);
    sd_ui_start(sd_D.menu_variant, l, 0);
}

static void menu_go_cb(lv_event_t *e)
{
    sd_snd_play(SD_S_TAP);
    sd_ui_go((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_menu(lv_obj_t *scr)
{
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W, H = sd_U.H;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P, bot = land ? BOT_L : BOT_P;
    int colw = land ? (W - 3 * SIDE) / 2 : W - 2 * SIDE;
    int x0 = SIDE, y = top;

    lv_obj_t *title = sd_label(scr, "Sudoku", &aos_inter_64, t->text);
    lv_obj_set_pos(title, x0 + 4, y - 4);
    coins_pill(scr, land ? x0 + colw : W - SIDE, y + 8);
    y += 92;

    /* the game in progress */
    if (sd_G.active) {
        lv_obj_t *c = button(scr, x0, y, colw, 112, t->accent, 255, menu_resume_cb, NULL);
        menu_add(c);
        lv_obj_t *a = sd_label(c, _("Seguir la partida"), aos_font_body, on_accent(t));
        lv_obj_align(a, LV_ALIGN_TOP_LEFT, 24, 18);
        char buf[96], tm[16];
        sd_fmt_time(tm, sizeof tm, sd_game_seconds());
        int filled = 0;
        for (int i = 0; i < sd_shape.ncells; i++)
            if (sd_G.val[i]) filled++;
        if (sd_G.mode == SD_M_DAILY)
            snprintf(buf, sizeof buf, "%s  ·  %s  ·  %d%%", _(SD_MODE_NAME[SD_M_DAILY]), tm,
                     filled * 100 / (sd_shape.ncells ? sd_shape.ncells : 81));
        else
            snprintf(buf, sizeof buf, "%s  ·  %s  ·  %s  ·  %d%%", _(SD_MODE_NAME[sd_G.mode]),
                     _(SD_LEVEL_NAME[sd_G.level]), tm, filled * 100 / (sd_shape.ncells ? sd_shape.ncells : 81));
        lv_obj_t *b = sd_label(c, buf, aos_font_small, on_accent(t));
        lv_obj_set_style_text_opa(b, 210, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 24, -18);
        lv_obj_t *p = sd_label(c, AOS_SYM_PLAY, &aos_sym_44, on_accent(t));
        lv_obj_align(p, LV_ALIGN_RIGHT_MID, -24, 0);
        y += 112 + 14;
    }

    /* the daily challenge */
    {
        lv_obj_t *c = button(scr, x0, y, colw, 118, t->card, t->card_opa, menu_daily_cb, NULL);
        menu_add(c);
        lv_obj_t *ic = sd_label(c, AOS_SYM_CALENDAR_TODAY, &aos_sym_44, t->accent);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *a = sd_label(c, _("Desafío del día"), aos_font_body, t->text);
        lv_obj_align(a, LV_ALIGN_TOP_LEFT, 84, 20);
        char buf[96];
        uint32_t ymd;
        int wday;
        bool clock = sd_today(&ymd, &wday);
        if (clock) {
            int m, l;
            sd_daily_pick(ymd, wday, &m, &l);
            snprintf(buf, sizeof buf, "%s  ·  %s", _(SD_MODE_NAME[m]), _(SD_LEVEL_NAME[l]));
        } else {
            snprintf(buf, sizeof buf, "%s", _("Esperando la hora..."));
        }
        lv_obj_t *b = sd_label(c, buf, aos_font_small, t->dim);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 84, -20);
        const char *right = NULL;
        char rb[48];
        if (clock && sd_D.daily_done == ymd) {
            char tm[16];
            sd_fmt_time(tm, sizeof tm, sd_D.daily_time_s);
            snprintf(rb, sizeof rb, _("Hecho en %s"), tm);
            right = rb;
        } else if (sd_D.daily_streak > 1) {
            snprintf(rb, sizeof rb, _("Racha %u"), (unsigned)sd_D.daily_streak);
            right = rb;
        }
        if (right) {
            lv_obj_t *r = sd_label(c, right, aos_font_small, t->accent);
            lv_obj_align(r, LV_ALIGN_RIGHT_MID, -22, 0);
        }
        y += 118 + 14;
    }

    /* the bottom row: shop, statistics, settings */
    int by = H - bot - 100;
    {
        static const char *const ICON[3] = { AOS_SYM_PALETTE, AOS_SYM_TROPHY, AOS_SYM_COG };
        static const char *const CAP[3] = { N_("Temas"), N_("Estadísticas"), N_("Ajustes") };
        static const int SCR[3] = { SD_SCR_SHOP, SD_SCR_STATS, SD_SCR_SETTINGS };
        int bw = (colw - 2 * 12) / 3;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = button(scr, x0 + i * (bw + 12), by, bw, 100, t->card, t->card_opa, menu_go_cb,
                                 (void *)(intptr_t)SCR[i]);
            menu_add(b);
            lv_obj_t *g = sd_label(b, ICON[i], &aos_sym_44, t->accent);
            lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 10);
            lv_obj_t *l = sd_label(b, _(CAP[i]), aos_font_caption, t->text);
            lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
        }
    }

    /* a new game: the variant, then the level */
    int nx = land ? x0 + colw + SIDE : x0;
    int ny = land ? top + 8 : y;
    lv_obj_t *nl = sd_label(scr, _("Partida nueva"), aos_font_small, t->dim);
    lv_obj_set_pos(nl, nx + 6, ny);
    ny += 40;
    int segw = (colw - 3 * 8) / 4;
    for (int v = 0; v < SD_M_DAILY; v++) {
        bool on = v == sd_D.menu_variant;
        lv_obj_t *b = button(scr, nx + v * (segw + 8), ny, segw, 76, on ? t->accent : t->card, on ? 255 : t->card_opa,
                             menu_variant_cb, (void *)(intptr_t)v);
        menu_add(b);
        lv_obj_t *l = sd_label(b, _(SD_MODE_NAME[v]), aos_font_small, on ? on_accent(t) : t->text);
        lv_obj_center(l);
    }
    ny += 76 + 12;
    int nlev = SD_MODE_LEVELS[sd_D.menu_variant];
    int limit = land ? H - bot : by - 14;
    int rh = (limit - ny - (nlev - 1) * 10) / nlev;
    if (rh > 100) rh = 100;
    int first_level = menu_n;
    for (int l = 0; l < nlev; l++) {
        lv_obj_t *r = button(scr, nx, ny, colw, rh, t->card, t->card_opa, menu_level_cb, (void *)(intptr_t)l);
        menu_add(r);
        /* a little bar of dots: how hard */
        for (int k = 0; k < 5; k++) {
            lv_obj_t *d = sd_box(r, k <= l ? t->accent : t->dim, k <= l ? 255 : 90, 6);
            lv_obj_set_size(d, 12, 12);
            lv_obj_align(d, LV_ALIGN_LEFT_MID, 24 + k * 18, 0);
        }
        lv_obj_t *n = sd_label(r, _(SD_LEVEL_NAME[l]), aos_font_body, t->text);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 24 + 5 * 18 + 16, 0);
        const sd_stat_t *s = &sd_D.st[sd_D.menu_variant][l];
        if (s->best_s) {
            char tm[16], buf[48];
            sd_fmt_time(tm, sizeof tm, s->best_s);
            snprintf(buf, sizeof buf, _("Mejor %s"), tm);
            lv_obj_t *b = sd_label(r, buf, aos_font_small, t->dim);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -24, 0);
        }
        ny += rh + 10;
    }
    menu_commit(sd_G.active ? 0 : first_level);
}

/* ======================================================================================
 * The game
 * ====================================================================================== */

static void panel_close(void)
{
    if (sd_U.panel && lv_obj_is_valid(sd_U.panel)) lv_obj_delete(sd_U.panel);
    sd_U.panel = NULL;
    sd_U.panel_kind = PANEL_NONE;
    if (sd_B.hint) {
        sd_B.hint = NULL;
        sd_board_all();
    }
    sd_B.paused = false;
    sd_board_present();
}

static void pad_digit_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (sd_U.panel_kind == PANEL_HINT) panel_close();
    if (sd_U.panel_kind != PANEL_NONE) return;
    sd_game_input(d);
    sd_board_present();
    sd_ui_refresh_game();
}

static void hint_show(void);
static void pause_show(void);

static void tool_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (sd_U.panel_kind == PANEL_HINT && k != 5) panel_close();
    if (sd_U.panel_kind != PANEL_NONE && sd_U.panel_kind != PANEL_HINT) return;
    switch (k) {
    case 0: sd_game_undo(); break;
    case 1: sd_game_redo(); break;
    case 2: sd_game_erase(); break;
    case 3:
        sd_G.notes_mode = !sd_G.notes_mode;
        sd_snd_play(SD_S_TAP);
        break;
    case 4: sd_game_autonotes(); break;
    case 5:
        if (sd_U.panel_kind == PANEL_HINT) panel_close();
        else hint_show();
        break;
    }
    sd_board_present();
    sd_ui_refresh_game();
}

static void pause_cb(lv_event_t *e)
{
    (void)e;
    if (sd_U.panel_kind == PANEL_PAUSE) panel_close();
    else if (sd_U.panel_kind == PANEL_NONE || sd_U.panel_kind == PANEL_HINT) pause_show();
    sd_ui_refresh_game();
}

void sd_ui_refresh_game(void)
{
    if (sd_U.screen != SD_SCR_GAME || !sd_U.lbl_time) return;
    const sd_theme_t *t = sd_theme();
    char buf[16];
    sd_fmt_time(buf, sizeof buf, sd_game_seconds());
    lv_label_set_text(sd_U.lbl_time, buf);
    for (int d = 1; d <= sd_shape.n; d++) {
        lv_obj_t *b = sd_U.pad_btn[d];
        if (!b) continue;
        int left = sd_game_remaining(d);
        bool chosen = sd_G.digit == d;
        lv_obj_set_style_bg_color(b, lv_color_hex(chosen ? t->accent : t->card), 0);
        lv_obj_set_style_bg_opa(b, chosen ? 255 : t->card_opa, 0);
        lv_obj_t *num = lv_obj_get_child(b, 0);
        if (num) {
            lv_obj_set_style_text_color(num, lv_color_hex(chosen ? on_accent(t) : t->text), 0);
            lv_obj_set_style_text_opa(num, left <= 0 && !chosen ? 90 : 255, 0);
        }
        if (sd_U.pad_cnt[d]) {
            if (left > 0) {
                snprintf(buf, sizeof buf, "%d", left);
                lv_obj_set_style_text_font(sd_U.pad_cnt[d], aos_font_caption, 0);
                lv_label_set_text(sd_U.pad_cnt[d], buf);
            } else {
                /* the caption font has no symbols: the check is in the glyph font */
                lv_obj_set_style_text_font(sd_U.pad_cnt[d], &aos_sym_28, 0);
                lv_label_set_text(sd_U.pad_cnt[d], AOS_SYM_CHECK);
            }
            lv_obj_set_style_text_color(sd_U.pad_cnt[d], lv_color_hex(chosen ? on_accent(t) : t->dim), 0);
        }
    }
    if (sd_U.btn_notes) {
        bool on = sd_G.notes_mode;
        lv_obj_set_style_bg_color(sd_U.btn_notes, lv_color_hex(on ? t->accent : t->card), 0);
        lv_obj_set_style_bg_opa(sd_U.btn_notes, on ? 255 : t->card_opa, 0);
        lv_obj_t *cap = lv_obj_get_child(sd_U.btn_notes, 1);
        if (cap) lv_label_set_text(cap, on ? _("Notas: sí") : _("Notas: no"));
    }
    if (sd_U.btn_tools[0]) {
        lv_obj_set_style_opa(sd_U.btn_tools[0], sd_G.nundo ? 255 : 110, 0);
        lv_obj_set_style_opa(sd_U.btn_tools[1], sd_G.nundo < sd_G.ntop ? 255 : 110, 0);
    }
}

/* where the panels go: over the tools and pad upright, the side lying down */
static void panel_area(int *x, int *y, int *w, int *h, bool over_board)
{
    if (over_board) {
        *x = sd_B.x;
        *y = sd_B.y;
        *w = sd_B.px;
        *h = sd_B.px;
        return;
    }
    bool land = sd_U.land;
    if (land) {
        *x = sd_B.x + sd_B.px + 24;
        *y = (land ? TOP_L : TOP_P) + 84 + 12;
        *w = sd_U.W - SIDE - *x;
        *h = sd_U.H - BOT_L - *y;
    } else {
        *x = SIDE;
        *y = sd_B.y + sd_B.px + 14;
        *w = sd_U.W - 2 * SIDE;
        *h = sd_U.H - BOT_P - *y;
    }
}

static lv_obj_t *panel_open(int kind, bool over_board)
{
    if (sd_U.panel) panel_close();
    const sd_theme_t *t = sd_theme();
    int x, y, w, h;
    panel_area(&x, &y, &w, &h, over_board);
    lv_obj_t *p = sd_box(sd_U.scr, mix(t->card, 0x000000, 40), 255, AOS_UI_RADIUS);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    sd_U.panel = p;
    sd_U.panel_kind = kind;
    menu_n = 0;
    return p;
}

static void hint_apply_cb(lv_event_t *e)
{
    (void)e;
    sd_step_t st = sd_U.hint;
    panel_close();
    sd_game_hint_apply(&st);
    sd_board_present();
    sd_ui_refresh_game();
}

static void panel_close_cb(lv_event_t *e)
{
    (void)e;
    panel_close();
    sd_ui_refresh_game();
}

static void hint_show(void)
{
    if (!sd_game_hint(&sd_U.hint)) return;
    sd_G.hints++;
    sd_snd_play(SD_S_HINT);
    const sd_theme_t *t = sd_theme();
    lv_obj_t *p = panel_open(PANEL_HINT, false);
    int w = lv_obj_get_style_width(p, 0), h = lv_obj_get_style_height(p, 0);
    lv_obj_t *ic = sd_label(p, AOS_SYM_LIGHTBULB, &aos_sym_44, t->hint);
    lv_obj_set_pos(ic, 20, 16);
    lv_obj_t *ti = sd_label(p, sd_tech_name(sd_U.hint.tech), aos_font_body, t->hint);
    lv_obj_set_pos(ti, 76, 22);
    char buf[400];
    sd_hint_text(&sd_U.hint, buf, sizeof buf);
    lv_obj_t *tx = sd_label(p, buf, aos_font_small, t->text);
    lv_obj_set_width(tx, w - 44);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(tx, 22, 74);
    int bh = 76, bw = (w - 3 * 18) / 2;
    lv_obj_t *a = button(p, 18, h - bh - 18, bw, bh, t->accent, 255, hint_apply_cb, NULL);
    lv_obj_center(sd_label(a, _("Aplicar"), aos_font_body, on_accent(t)));
    lv_obj_t *c = button(p, 18 * 2 + bw, h - bh - 18, bw, bh, t->card, 255, panel_close_cb, NULL);
    lv_obj_center(sd_label(c, _("Cerrar"), aos_font_body, t->text));
    menu_add(a);
    menu_add(c);
    menu_commit(0);
    sd_B.hint = &sd_U.hint;
    sd_board_all();
    sd_board_present();
}

static void pause_resume_cb(lv_event_t *e)
{
    (void)e;
    panel_close();
    sd_ui_refresh_game();
}

static void pause_menu_cb(lv_event_t *e)
{
    (void)e;
    panel_close();
    sd_game_save();
    sd_ui_go(SD_SCR_MENU);
}

static void pause_restart_cb(lv_event_t *e)
{
    (void)e;
    panel_close();
    /* back to the givens, the clock and the counters kept: no free coins */
    for (int c = 0; c < sd_shape.ncells; c++) {
        sd_G.val[c] = sd_G.puz.given[c];
        sd_G.notes[c] = 0;
        sd_G.hint_elim[c] = 0;
    }
    sd_G.nundo = sd_G.ntop = 0;
    sd_G.sel = -1;
    sd_board_all();
    sd_board_present();
    sd_ui_refresh_game();
}

static void pause_tool_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    panel_close();
    if (k == 5) hint_show();
    else if (k == 0) sd_game_undo();
    else if (k == 4) sd_game_autonotes();
    sd_board_present();
    sd_ui_refresh_game();
}

static void pause_show(void)
{
    const sd_theme_t *t = sd_theme();
    lv_obj_t *p = panel_open(PANEL_PAUSE, true);
    sd_B.paused = true;
    int w = sd_B.px, h = sd_B.px;
    lv_obj_set_style_bg_color(p, lv_color_hex(t->card), 0);
    lv_obj_set_style_bg_opa(p, 255, 0);
    lv_obj_t *ti = sd_label(p, _("Pausa"), &aos_inter_64, t->text);
    lv_obj_align(ti, LV_ALIGN_TOP_MID, 0, h / 12);
    char tm[16];
    sd_fmt_time(tm, sizeof tm, sd_game_seconds());
    lv_obj_t *tl = sd_label(p, tm, aos_font_title, t->dim);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, h / 12 + 84);
    int bw = w * 3 / 4, bh = (h - h / 12 - 150 - 5 * 12 - 20) / 5;
    if (bh > 76) bh = 76;
    int y = h / 12 + 150;
    static const char *const CAP[5] = { N_("Seguir"), N_("Pista"), N_("Deshacer"), N_("Reiniciar"), N_("Menú") };
    for (int i = 0; i < 5; i++) {
        lv_event_cb_t cb = i == 0 ? pause_resume_cb : i == 3 ? pause_restart_cb : i == 4 ? pause_menu_cb : pause_tool_cb;
        void *ud = (void *)(intptr_t)(i == 1 ? 5 : 0);
        lv_obj_t *b = button(p, (w - bw) / 2, y, bw, bh, i == 0 ? t->accent : mix(t->card, 0xFFFFFF, 26), 255, cb, ud);
        lv_obj_center(sd_label(b, _(CAP[i]), aos_font_body, i == 0 ? on_accent(t) : t->text));
        menu_add(b);
        y += bh + 12;
    }
    menu_commit(0);
}

static void wait_show(void)
{
    const sd_theme_t *t = sd_theme();
    lv_obj_t *p = panel_open(PANEL_WAIT, true);
    sd_B.paused = true;
    lv_obj_set_style_bg_opa(p, 255, 0);
    lv_obj_t *sp = lv_spinner_create(p);
    lv_obj_set_size(sp, 110, 110);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_arc_color(sp, lv_color_hex(t->accent), LV_PART_INDICATOR);
    lv_obj_t *l = sd_label(p, _("Preparando el tablero..."), aos_font_body, t->text);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 60);
    lv_obj_t *l2 = sd_label(p, _("Buscando uno que pida justo esas técnicas"), aos_font_small, t->dim);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 104);
}

static void won_again_cb(lv_event_t *e)
{
    (void)e;
    int m = sd_G.mode, l = sd_G.level;
    if (m == SD_M_DAILY) {
        sd_ui_go(SD_SCR_MENU);
        return;
    }
    sd_ui_start(m, l, 0);
}

void sd_ui_won(void)
{
    if (sd_U.screen != SD_SCR_GAME) return;
    const sd_theme_t *t = sd_theme();
    lv_obj_t *p = panel_open(PANEL_WON, false);
    int w = lv_obj_get_style_width(p, 0), h = lv_obj_get_style_height(p, 0);
    char buf[64], tm[16];
    lv_obj_t *ti = sd_label(p, _("¡Resuelto!"), aos_font_title, t->accent);
    lv_obj_set_pos(ti, 24, 16);
    sd_fmt_time(tm, sizeof tm, sd_game_seconds());
    snprintf(buf, sizeof buf, sd_G.record ? _("%s, récord") : "%s", tm);
    lv_obj_t *tl = sd_label(p, buf, aos_font_body, t->text);
    lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -24, 22);

    static const char *const WHY[6] = { N_("Base"), N_("Sin pistas"), N_("Sin errores"), N_("A tiempo"),
                                        N_("Sin ayudas"), N_("Racha de días") };
    int y = 74, lh = 34;
    int bh = 76;
    int room = h - y - bh - 30 - 44;
    int rows = 0;
    for (int i = 0; i < 6; i++)
        if (sd_G.pay[i]) rows++;
    if (rows * lh > room) lh = room / (rows ? rows : 1);
    for (int i = 0; i < 6; i++) {
        if (!sd_G.pay[i]) continue;
        lv_obj_t *a = sd_label(p, _(WHY[i]), aos_font_small, t->dim);
        lv_obj_set_pos(a, 24, y);
        snprintf(buf, sizeof buf, "+%u", (unsigned)sd_G.pay[i]);
        lv_obj_t *b = sd_label(p, buf, aos_font_small, t->text);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, y);
        y += lh;
    }
    lv_obj_t *g = glyph(p, GL_COIN, 34, 0);
    lv_obj_set_pos(g, 24, y + 6);
    snprintf(buf, sizeof buf, _("%u monedas"), (unsigned)sd_G.coins_won);
    lv_obj_t *tot = sd_label(p, buf, aos_font_body, t->text);
    lv_obj_set_pos(tot, 70, y + 4);

    int bw = (w - 3 * 18) / 2;
    lv_obj_t *a = button(p, 18, h - bh - 18, bw, bh, t->accent, 255, won_again_cb, NULL);
    lv_obj_center(sd_label(a, sd_G.mode == SD_M_DAILY ? _("Listo") : _("Otra igual"), aos_font_body, on_accent(t)));
    lv_obj_t *c = button(p, 2 * 18 + bw, h - bh - 18, bw, bh, t->card, 255, pause_menu_cb, NULL);
    lv_obj_center(sd_label(c, _("Menú"), aos_font_body, t->text));
    menu_add(a);
    menu_add(c);
    menu_commit(0);
    sd_ui_refresh_game();
}

static void build_game(lv_obj_t *scr)
{
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W, H = sd_U.H;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P, bot = land ? BOT_L : BOT_P;
    int n = sd_shape.n ? sd_shape.n : 9;

    /* the board's place */
    int bx, by, bmax;
    if (land) {
        bmax = H - top - bot;
        bx = SIDE + 6;
        by = top;
    } else {
        bmax = W - 2 * SIDE + 12;
        bx = SIDE - 6;
        by = top + 84 + 10;
    }
    sd_board_create(scr, bx, by, bmax);

    /* the header */
    int hx = land ? sd_B.x + sd_B.px + 24 : SIDE, hw = W - SIDE - hx;
    lv_obj_t *b = button(scr, hx, top + 6, 64, 64, t->card, t->card_opa, back_cb, NULL);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_center(sd_label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, t->text));
    char buf[80];
    if (sd_G.mode == SD_M_DAILY) snprintf(buf, sizeof buf, "%s", _(SD_MODE_NAME[SD_M_DAILY]));
    else snprintf(buf, sizeof buf, "%s · %s", _(SD_MODE_NAME[sd_G.mode]), _(SD_LEVEL_NAME[sd_G.level]));
    /* on a pill: the picture behind may be as light as the text (a moon) */
    lv_obj_t *pill = sd_box(scr, t->card, t->card_opa, 22);
    lv_obj_set_pos(pill, hx + 74, top + 16);
    lv_obj_set_height(pill, 44);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(pill, 16, 0);
    sd_U.lbl_title = sd_label(pill, buf, aos_font_small, t->text);
    lv_obj_align(sd_U.lbl_title, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *pb = button(scr, hx + hw - 64, top + 6, 64, 64, t->card, t->card_opa, pause_cb, NULL);
    lv_obj_set_style_radius(pb, 32, 0);
    lv_obj_center(sd_label(pb, AOS_SYM_PAUSE, &aos_sym_44, t->text));
    sd_U.lbl_time = sd_label(scr, "0:00", aos_font_title, t->text);
    lv_obj_align(sd_U.lbl_time, LV_ALIGN_TOP_RIGHT, -(W - (hx + hw - 64)) - 14, top + 14);

    /* the tools */
    int ty, th, tx, tw;
    if (land) {
        tx = hx;
        tw = hw;
        ty = top + 84 + 12;
        th = 100;
    } else {
        tx = SIDE;
        tw = W - 2 * SIDE;
        ty = sd_B.y + sd_B.px + 14;
        th = 104;
    }
    {
        static const char *const CAP[6] = { N_("Deshacer"), N_("Rehacer"), N_("Borrar"), N_("Notas: no"),
                                            N_("Auto"), N_("Pista") };
        int gap = 8, bw = (tw - 5 * gap) / 6;
        for (int i = 0; i < 6; i++) {
            lv_obj_t *o = button(scr, tx + i * (bw + gap), ty, bw, th, t->card, t->card_opa, tool_cb,
                                 (void *)(intptr_t)i);
            sd_U.btn_tools[i] = o;
            lv_obj_t *g;
            if (i == 0) g = glyph(o, GL_UNDO, 44, t->text);
            else if (i == 1) g = glyph(o, GL_REDO, 44, t->text);
            else {
                static const char *const SYM[6] = { "", "", AOS_SYM_ERASER, AOS_SYM_PENCIL, AOS_SYM_AUTO_FIX,
                                                    AOS_SYM_LIGHTBULB_OUTLINE };
                g = sd_label(o, SYM[i], &aos_sym_44, i == 5 ? t->hint : t->text);
            }
            lv_obj_align(g, LV_ALIGN_TOP_MID, 0, th / 2 - 44);
            lv_obj_t *l = sd_label(o, _(CAP[i]), aos_font_caption, t->text);
            lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
            if (i == 3) sd_U.btn_notes = o;
        }
    }

    /* the number pad */
    memset(sd_U.pad_btn, 0, sizeof sd_U.pad_btn);
    memset(sd_U.pad_cnt, 0, sizeof sd_U.pad_cnt);
    int py = ty + th + 14, ph = H - bot - py;
    if (land) {
        int cols = 3, rows = (n + 2) / 3, gap = 10;
        int bw = (tw - (cols - 1) * gap) / cols, bh = (ph - (rows - 1) * gap) / rows;
        if (bh > 150) bh = 150;
        for (int d = 1; d <= n; d++) {
            int k = d - 1;
            lv_obj_t *o = button(scr, tx + (k % cols) * (bw + gap), py + (k / cols) * (bh + gap), bw, bh, t->card,
                                 t->card_opa, pad_digit_cb, (void *)(intptr_t)d);
            sd_U.pad_btn[d] = o;
            lv_obj_t *num = sd_label(o, DIGITS[d], &aos_inter_64, t->text);
            lv_obj_align(num, LV_ALIGN_CENTER, -16, 0);
            sd_U.pad_cnt[d] = sd_label(o, "", aos_font_caption, t->dim);
            lv_obj_align(sd_U.pad_cnt[d], LV_ALIGN_BOTTOM_RIGHT, -14, -10);
        }
    } else {
        int gap = n == 9 ? 6 : 10, bw = (tw - (n - 1) * gap) / n, bh = ph > 160 ? 160 : ph;
        py += (ph - bh) / 2;
        for (int d = 1; d <= n; d++) {
            lv_obj_t *o = button(scr, tx + (d - 1) * (bw + gap), py, bw, bh, t->card, t->card_opa, pad_digit_cb,
                                 (void *)(intptr_t)d);
            sd_U.pad_btn[d] = o;
            lv_obj_t *num = sd_label(o, DIGITS[d], &aos_inter_64, t->text);
            lv_obj_align(num, LV_ALIGN_CENTER, 0, -14);
            sd_U.pad_cnt[d] = sd_label(o, "", aos_font_caption, t->dim);
            lv_obj_align(sd_U.pad_cnt[d], LV_ALIGN_BOTTOM_MID, 0, -10);
        }
    }
    aos_pad_menu_clear(&sd_U.menu);
    sd_ui_refresh_game();
    if (sd_U.waiting) wait_show();
    else if (sd_G.won) sd_ui_won();
}

/* ======================================================================================
 * The shop and the preview
 * ====================================================================================== */

static void shop_pick_cb(lv_event_t *e)
{
    sd_U.preview = (int)(intptr_t)lv_event_get_user_data(e);
    sd_snd_play(SD_S_TAP);
    sd_ui_go(SD_SCR_PREVIEW);
}

static void shop_thumbs(void)
{
    for (int i = 0; i < SD_THEMES; i++) {
        lv_obj_t *o = sd_U.thumbs[i];
        if (!o || !lv_obj_is_valid(o) || !sd_A.thumb[i]) continue;
        lv_image_dsc_t *d = &sd_U.thumb_dsc[i];
        if (d->data == (const uint8_t *)sd_A.thumb[i]) continue;
        memset(d, 0, sizeof *d);
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = SD_THUMB;
        d->header.h = SD_THUMB;
        d->header.stride = SD_THUMB * 2;
        d->data_size = SD_THUMB * SD_THUMB * 2;
        d->data = (const uint8_t *)sd_A.thumb[i];
        lv_obj_t *img = lv_image_create(o);
        lv_image_set_src(img, d);
        lv_obj_center(img);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void build_shop(lv_obj_t *scr)
{
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W, H = sd_U.H;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P, bot = land ? BOT_L : BOT_P;
    header(scr, top, _("Temas"), true);
    sd_theme_thumbs();

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, top + 80);
    lv_obj_set_size(list, W, H - top - 80);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    int cols = land ? 5 : 3, gap = 14;
    int cw = (W - 2 * SIDE - (cols - 1) * gap) / cols, ch = SD_THUMB + 20 + 80;
    memset(sd_U.thumbs, 0, sizeof sd_U.thumbs);
    memset(sd_U.thumb_dsc, 0, sizeof sd_U.thumb_dsc);
    int cur = 0;
    for (int i = 0; i < SD_THEMES; i++) {
        const sd_theme_t *th = &SD_THEME[i];
        int x = SIDE + (i % cols) * (cw + gap), y = 8 + (i / cols) * (ch + gap);
        lv_obj_t *c = button(list, x, y, cw, ch, t->card, t->card_opa, shop_pick_cb, (void *)(intptr_t)i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        if (i == sd_D.theme) {
            lv_obj_set_style_border_color(c, lv_color_hex(t->accent), 0);
            lv_obj_set_style_border_width(c, 4, 0);
            lv_obj_set_style_border_opa(c, 255, 0);
            cur = i;
        }
        menu_add(c);
        /* the thumbnail, or the theme's two colours until it comes */
        lv_obj_t *tb = sd_box(c, th->bg, 255, 16);
        lv_obj_set_size(tb, SD_THUMB, SD_THUMB);
        lv_obj_align(tb, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_clip_corner(tb, true, 0);
        lv_obj_t *mini = sd_box(tb, th->board, 255, 8);
        lv_obj_set_size(mini, 120, 120);
        lv_obj_center(mini);
        sd_U.thumbs[i] = tb;
        lv_obj_t *nm = sd_label(c, _(th->name), aos_font_small, t->text);
        lv_obj_set_width(nm, cw - 16);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -42);
        char buf[32];
        bool own = sd_D.owned & (1u << i);
        if (i == sd_D.theme) snprintf(buf, sizeof buf, "%s", _("En uso"));
        else if (own) snprintf(buf, sizeof buf, "%s", _("Tuyo"));
        else snprintf(buf, sizeof buf, "%u", (unsigned)th->price);
        lv_obj_t *pr = sd_label(c, buf, aos_font_small, own ? t->accent : t->text);
        lv_obj_align(pr, LV_ALIGN_BOTTOM_MID, own ? 0 : 18, -10);
        if (!own) {
            lv_obj_t *g = glyph(c, GL_COIN, 26, 0);
            lv_obj_align_to(g, pr, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        }
    }
    lv_obj_t *pad = sd_box(list, 0, 0, 0);
    lv_obj_set_size(pad, 10, bot + 20);
    lv_obj_set_pos(pad, 0, 8 + ((SD_THEMES + cols - 1) / cols) * (ch + gap));
    shop_thumbs();
    menu_commit(1 + cur);
}

static void preview_buy_cb(lv_event_t *e)
{
    (void)e;
    int i = sd_U.preview;
    const sd_theme_t *th = &SD_THEME[i];
    bool own = sd_D.owned & (1u << i);
    if (!own) {
        if (sd_D.coins < th->price) return;
        sd_D.coins -= th->price;
        sd_D.owned |= 1u << i;
        sd_snd_play(SD_S_WIN);
        sd_ui_toast(_("¡Es tuyo!"));
    } else {
        sd_snd_play(SD_S_TAP);
    }
    sd_D.theme = (uint8_t)i;
    sd_data_save();
    sd_ui_build();
}

static void build_preview(lv_obj_t *scr)
{
    int i = sd_U.preview;
    const sd_theme_t *th = &SD_THEME[i];
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W, H = sd_U.H;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P, bot = land ? BOT_L : BOT_P;
    header(scr, top, _(th->name), true);
    sd_board_demo(true);
    int bmax, bx, by;
    if (land) {
        bmax = H - top - bot - 80;
        bx = SIDE;
        by = top + 80;
    } else {
        bmax = W - 2 * SIDE;
        bx = SIDE;
        by = top + 96;
    }
    sd_board_create(scr, bx, by, bmax);

    int cx, cy, cw, chh;
    if (land) {
        cx = sd_B.x + sd_B.px + 24;
        cy = top + 80;
        cw = W - SIDE - cx;
        chh = H - bot - cy;
    } else {
        cx = SIDE;
        cy = sd_B.y + sd_B.px + 20;
        cw = W - 2 * SIDE;
        chh = H - bot - cy;
    }
    lv_obj_t *c = card(scr, cx, cy, cw, chh);
    bool own = sd_D.owned & (1u << i), in_use = sd_D.theme == i;
    char buf[96];
    if (in_use) snprintf(buf, sizeof buf, "%s", _("Es el tema que estás usando."));
    else if (own) snprintf(buf, sizeof buf, "%s", _("Ya es tuyo: ponelo cuando quieras."));
    else if (th->price == 0) snprintf(buf, sizeof buf, "%s", _("Gratis."));
    else snprintf(buf, sizeof buf, _("Cuesta %u monedas. Tenés %u."), (unsigned)th->price, (unsigned)sd_D.coins);
    lv_obj_t *d = sd_label(c, buf, aos_font_body, t->text);
    lv_obj_set_width(d, cw - 48);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d, 24, 22);
    lv_obj_t *d2 = sd_label(c, _("El fondo, las celdas, los números, las notas y el sonido cambian con el tema."),
                            aos_font_small, t->dim);
    lv_obj_set_width(d2, cw - 48);
    lv_label_set_long_mode(d2, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d2, 24, 100);

    int bh = 84, bw = cw - 48;
    lv_obj_t *b;
    if (in_use) {
        b = button(c, 24, chh - bh - 22, bw, bh, t->card, 255, back_cb, NULL);
        lv_obj_center(sd_label(b, _("Volver"), aos_font_body, t->text));
    } else if (own || th->price == 0) {
        b = button(c, 24, chh - bh - 22, bw, bh, t->accent, 255, preview_buy_cb, NULL);
        lv_obj_center(sd_label(b, _("Usar este tema"), aos_font_body, on_accent(t)));
    } else if (sd_D.coins >= th->price) {
        b = button(c, 24, chh - bh - 22, bw, bh, t->accent, 255, preview_buy_cb, NULL);
        snprintf(buf, sizeof buf, _("Comprar por %u"), (unsigned)th->price);
        lv_obj_t *l = sd_label(b, buf, aos_font_body, on_accent(t));
        lv_obj_align(l, LV_ALIGN_CENTER, 20, 0);
        lv_obj_t *g = glyph(b, GL_COIN, 34, 0);
        lv_obj_align_to(g, l, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    } else {
        b = button(c, 24, chh - bh - 22, bw, bh, t->card, 160, NULL, NULL);
        snprintf(buf, sizeof buf, _("Te faltan %u monedas"), (unsigned)(th->price - sd_D.coins));
        lv_obj_center(sd_label(b, buf, aos_font_body, t->dim));
    }
    menu_add(b);
    menu_commit(menu_n - 1);
}

/* ======================================================================================
 * Statistics
 * ====================================================================================== */

static void build_stats(lv_obj_t *scr)
{
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W, H = sd_U.H;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P, bot = land ? BOT_L : BOT_P;
    header(scr, top, _("Estadísticas"), true);
    menu_commit(0);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, top + 80);
    lv_obj_set_size(list, W, H - top - 80);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    int cols = land ? 2 : 1, gap = 14, cw = (W - 2 * SIDE - (cols - 1) * gap) / cols;
    int colh[2] = { 8, 8 };
    char buf[128];

    /* the totals */
    {
        int h = 160, k = 0;
        lv_obj_t *c = card(list, SIDE, colh[k], cw, h);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *g = glyph(c, GL_COIN, 40, 0);
        lv_obj_set_pos(g, 24, 22);
        snprintf(buf, sizeof buf, _("%u monedas ganadas en total"), (unsigned)sd_D.coins_total);
        lv_obj_set_pos(sd_label(c, buf, aos_font_body, t->text), 80, 26);
        snprintf(buf, sizeof buf, _("Racha: %u días seguidos (la mejor, %u)"), (unsigned)sd_D.streak,
                 (unsigned)sd_D.best_streak);
        lv_obj_set_pos(sd_label(c, buf, aos_font_small, t->dim), 24, 80);
        snprintf(buf, sizeof buf, _("Desafío del día: %u seguidos"), (unsigned)sd_D.daily_streak);
        lv_obj_set_pos(sd_label(c, buf, aos_font_small, t->dim), 24, 116);
        colh[k] += h + gap;
    }
    for (int m = 0; m < SD_MODES; m++) {
        int nl = SD_MODE_LEVELS[m];
        int h = 70 + nl * 78;
        int k = cols == 2 ? (colh[0] <= colh[1] ? 0 : 1) : 0;
        lv_obj_t *c = card(list, SIDE + k * (cw + gap), colh[k], cw, h);
        lv_obj_set_pos(sd_label(c, _(SD_MODE_NAME[m]), aos_font_body, t->accent), 24, 18);
        for (int l = 0; l < nl; l++) {
            const sd_stat_t *s = &sd_D.st[m][l];
            int y = 66 + l * 78;
            if (m != SD_M_DAILY) lv_obj_set_pos(sd_label(c, _(SD_LEVEL_NAME[l]), aos_font_small, t->text), 24, y);
            snprintf(buf, sizeof buf, _("Jugadas: %u · ganadas: %u"), (unsigned)s->played, (unsigned)s->won);
            lv_obj_t *a = sd_label(c, buf, aos_font_small, t->dim);
            lv_obj_align(a, LV_ALIGN_TOP_RIGHT, -24, y);
            if (s->won) {
                char b1[16], b2[16];
                sd_fmt_time(b1, sizeof b1, s->best_s);
                sd_fmt_time(b2, sizeof b2, s->sum_s / s->won);
                snprintf(buf, sizeof buf, _("Mejor %s · promedio %s"), b1, b2);
            } else {
                snprintf(buf, sizeof buf, "%s", _("Sin tiempos todavía"));
            }
            lv_obj_t *b = sd_label(c, buf, aos_font_caption, t->dim);
            lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, y + 34);
        }
        colh[k] += h + gap;
    }
    int maxh = colh[0] > colh[1] ? colh[0] : colh[1];
    lv_obj_t *pad = sd_box(list, 0, 0, 0);
    lv_obj_set_size(pad, 10, bot + 10);
    lv_obj_set_pos(pad, 0, maxh);
}

/* ======================================================================================
 * Settings
 * ====================================================================================== */

static bool *setting_ptr(int k)
{
    switch (k) {
    case 0: return &sd_D.mark_errors;
    case 1: return &sd_D.hl_same;
    case 2: return &sd_D.hl_peers;
    case 3: return &sd_D.auto_clean;
    case 4: return &sd_D.sound;
    default: return &sd_D.anim;
    }
}

static void setting_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    bool *p = setting_ptr(k);
    *p = !*p;
    sd_data_save();
    if (k == 4 && *p) sd_snd_open();
    if (k == 5) sd_ui_set_bg();
    if (k == 0 && *p && sd_G.active) sd_G.assisted = true;
    sd_snd_play(SD_S_TAP);
    sd_ui_build();
}

static void build_settings(lv_obj_t *scr)
{
    const sd_theme_t *t = sd_theme();
    int W = sd_U.W;
    bool land = sd_U.land;
    int top = land ? TOP_L : TOP_P;
    header(scr, top, _("Ajustes"), false);
    static const char *const NAME[6] = { N_("Marcar los errores"), N_("Resaltar los números iguales"),
                                         N_("Resaltar fila, columna y caja"), N_("Borrar las notas solas"),
                                         N_("Sonido"), N_("Fondo animado") };
    static const char *const SUB[6] = {
        N_("Sin marcarlos, cada partida paga más"), N_("Y las notas de ese número"), N_("Las de la celda elegida"),
        N_("Al poner un número, sale de las notas que lo ven"), N_("Con el sonido de cada tema"),
        N_("Burbujas, fantasmas, nieve...: según el tema"),
    };
    int cols = land ? 2 : 1, gap = 14, cw = (W - 2 * SIDE - (cols - 1) * gap) / cols, rh = land ? 120 : 132;
    for (int k = 0; k < 6; k++) {
        int x = SIDE + (k % cols) * (cw + gap), y = top + 92 + (k / cols) * (rh + gap);
        lv_obj_t *r = button(sd_U.scr, x, y, cw, rh, t->card, t->card_opa, setting_cb, (void *)(intptr_t)k);
        menu_add(r);
        lv_obj_t *a = sd_label(r, _(NAME[k]), aos_font_body, t->text);
        lv_obj_set_width(a, cw - 170);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(a, 24, 20);
        lv_obj_t *b = sd_label(r, _(SUB[k]), aos_font_caption, t->dim);
        lv_obj_set_width(b, cw - 170);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(b, 24, 62);
        /* a switch of our own: a pill with a knob */
        bool on = *setting_ptr(k);
        lv_obj_t *sw = sd_box(r, on ? t->accent : t->dim, on ? 255 : 120, 26);
        lv_obj_set_size(sw, 96, 52);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_t *kn = sd_box(sw, 0xFFFFFF, 255, 22);
        lv_obj_set_size(kn, 44, 44);
        lv_obj_align(kn, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -4 : 4, 0);
    }
    (void)scr;
    menu_commit(1);
}

/* ======================================================================================
 * The screens, switched
 * ====================================================================================== */

void sd_ui_set_bg(void)
{
    if (!sd_U.root) return;
    const sd_theme_t *t = sd_theme();
    lv_obj_set_style_bg_color(sd_U.root, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(sd_U.root, 255, 0);
    bool have = sd_A.bg && sd_A.theme == (int)(t - SD_THEME);
    if (have) {
        lv_image_dsc_t *d = &sd_U.bg_dsc;
        memset(d, 0, sizeof *d);
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = SD_BG_W;
        d->header.h = SD_BG_H;
        d->header.stride = SD_BG_W * 2;
        d->data_size = SD_BG_W * SD_BG_H * 2;
        d->data = (const uint8_t *)sd_A.bg;
        lv_image_set_src(sd_U.bgimg, d);
        lv_obj_set_pos(sd_U.bgimg, (sd_U.W - SD_BG_W) / 2, (sd_U.H - SD_BG_H) / 2);
        lv_obj_remove_flag(sd_U.bgimg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sd_U.bgimg, LV_OBJ_FLAG_HIDDEN);
    }
    if (sd_D.anim && have) sd_decor_build(sd_U.decor);
    else sd_decor_clear();
}

void sd_ui_build(void)
{
    if (!sd_U.scr) return;
    sd_board_destroy();
    lv_obj_clean(sd_U.scr);
    sd_U.panel = NULL;
    sd_U.panel_kind = PANEL_NONE;
    sd_U.lbl_time = sd_U.lbl_title = sd_U.lbl_coins = NULL;
    sd_U.btn_notes = NULL;
    memset(sd_U.btn_tools, 0, sizeof sd_U.btn_tools);
    memset(sd_U.pad_btn, 0, sizeof sd_U.pad_btn);
    memset(sd_U.thumbs, 0, sizeof sd_U.thumbs);
    sd_B.hint = NULL;
    sd_B.paused = false;
    menu_n = 0;
    aos_pad_menu_clear(&sd_U.menu);
    sd_U.menu.shown = false;

    int want = sd_U.screen == SD_SCR_PREVIEW ? sd_U.preview : sd_D.theme;
    if (sd_theme() != &SD_THEME[want] || !sd_theme_ready()) sd_theme_show(want);
    sd_board_demo(sd_U.screen == SD_SCR_PREVIEW);
    sd_ui_set_bg();

    lv_obj_t *scr = sd_U.scr;
    switch (sd_U.screen) {
    case SD_SCR_GAME:
        if (!sd_G.active && !sd_G.won && !sd_U.waiting) {
            sd_U.screen = SD_SCR_MENU;
            build_menu(scr);
        } else {
            build_game(scr);
        }
        break;
    case SD_SCR_SHOP: build_shop(scr); break;
    case SD_SCR_PREVIEW: build_preview(scr); break;
    case SD_SCR_STATS: build_stats(scr); break;
    case SD_SCR_SETTINGS: build_settings(scr); break;
    default: build_menu(scr); break;
    }
}

void sd_ui_go(int screen)
{
    if (sd_U.screen == SD_SCR_GAME && screen != SD_SCR_GAME) sd_game_save();
    sd_U.screen = screen;
    sd_ui_build();
}

void sd_ui_start(int mode, int level, uint32_t daily)
{
    sd_packed_t p;
    if (sd_G.active && !sd_G.won && !(sd_G.mode == mode && sd_G.level == level && sd_G.daily == daily && daily)) {
        /* a new game drops the one in progress: it counted as played already */
    }
    if (daily && sd_G.active && sd_G.daily == daily) {
        sd_ui_go(SD_SCR_GAME);
        return;
    }
    sd_U.want_mode = mode;
    sd_U.want_level = level;
    sd_U.want_daily = daily;
    if (sd_gen_take(mode, level, daily, &p)) {
        sd_U.waiting = false;
        sd_game_new(mode, level, daily, &p);
    } else {
        sd_U.waiting = true;
        memset(&sd_G, 0, sizeof sd_G);
        sd_game_forget();
        sd_G.puz.n = (uint8_t)(mode == SD_M_MINI ? 6 : 9);
        sd_G.puz.diag = mode == SD_M_DIAG;
        sd_unpack_shape(&sd_G.puz, &sd_shape);
        sd_G.mode = (uint8_t)mode;
        sd_G.level = (uint8_t)level;
        sd_G.sel = -1;
    }
    sd_ui_go(SD_SCR_GAME);
}

bool sd_ui_back(void)
{
    switch (sd_U.screen) {
    case SD_SCR_MENU: return false;
    case SD_SCR_GAME:
        if (sd_U.panel_kind == PANEL_HINT || sd_U.panel_kind == PANEL_PAUSE) {
            panel_close();
            sd_ui_refresh_game();
            return true;
        }
        sd_U.waiting = false;
        sd_ui_go(SD_SCR_MENU);
        return true;
    case SD_SCR_PREVIEW: sd_ui_go(SD_SCR_SHOP); return true;
    default: sd_ui_go(SD_SCR_MENU); return true;
    }
}

/* ======================================================================================
 * Every 30 ms
 * ====================================================================================== */

void sd_ui_frame(void)
{
    uint32_t now = lv_tick_get();
    uint32_t dt = now - sd_U.last_ms;
    sd_U.last_ms = now;
    if (dt > 1000) dt = 0;

    sd_snd_pump();
    if (sd_theme_poll() && sd_U.screen == SD_SCR_SHOP) shop_thumbs();
    sd_decor_step(dt);

    if (sd_U.screen == SD_SCR_GAME) {
        if (sd_U.waiting) {
            sd_packed_t p;
            if (sd_gen_poll(&p)) {
                sd_U.waiting = false;
                sd_game_new(sd_U.want_mode, sd_U.want_level, sd_U.want_daily, &p);
                sd_ui_build();
            }
        } else if (sd_U.panel_kind != PANEL_PAUSE && sd_U.panel_kind != PANEL_WON) {
            uint32_t s0 = sd_game_seconds();
            sd_game_tick(dt);
            if (sd_game_seconds() != s0) {
                sd_ui_refresh_game();
                /* a save now and then: a power cut loses a minute at most */
                if (sd_game_seconds() % 60 == 0) sd_game_save();
            }
        }
    }
    sd_ui_pad();
}

/* ---- the gamepad and the keyboard -------------------------------------------------------- */

static void cursor_move(int dx, int dy)
{
    int n = sd_shape.n;
    int c = sd_G.sel < 0 ? (n / 2) * n + n / 2 : sd_G.sel;
    if (sd_B.cursor || sd_G.sel >= 0) {
        int r = c / n + dy, col = c % n + dx;
        if (r < 0) r = n - 1;
        if (r >= n) r = 0;
        if (col < 0) col = n - 1;
        if (col >= n) col = 0;
        c = r * n + col;
    }
    sd_B.cursor = true;
    sd_G.sel = (int8_t)c;
    if (sd_G.val[c]) sd_G.digit = sd_G.val[c];
    sd_board_all();
}

static void digit_step(int dir)
{
    int n = sd_shape.n, d = sd_G.digit;
    for (int k = 0; k < n; k++) {
        d += dir;
        if (d < 1) d = n;
        if (d > n) d = 1;
        if (sd_game_remaining(d) > 0) break;
    }
    sd_G.digit = (uint8_t)d;
    sd_board_all();
    sd_snd_play(SD_S_TAP);
}

void sd_ui_pad(void)
{
    aos_pad_t *p = &sd_U.pad;
    aos_pad_update(p, lv_tick_get());
    /* the keyboard's arrows, Enter and Escape arrive as presses */
    if (sd_U.kb_bits) {
        p->pressed |= sd_U.kb_bits;
        p->repeat |= sd_U.kb_bits;
        sd_U.kb_bits = 0;
    }
    if (!p->pressed && !p->repeat) return;

    bool game = sd_U.screen == SD_SCR_GAME && sd_U.panel_kind == PANEL_NONE && sd_G.active && !sd_U.waiting;
    if (!game) {
        if (aos_pad_pressed(p, AOS_PAD_B)) {
            sd_ui_back();
            return;
        }
        if (sd_U.screen == SD_SCR_GAME && sd_U.panel_kind == PANEL_PAUSE && aos_pad_pressed(p, AOS_PAD_START)) {
            panel_close();
            sd_ui_refresh_game();
            return;
        }
        if (aos_pad_pressed(p, AOS_PAD_START) && sd_U.panel_kind == PANEL_NONE && sd_U.screen == SD_SCR_MENU &&
            sd_G.active) {
            sd_ui_go(SD_SCR_GAME);
            return;
        }
        aos_pad_menu_step(&sd_U.menu, p);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        pause_show();
        sd_ui_refresh_game();
        return;
    }
    uint32_t dirs = p->repeat & AOS_PAD_DIRS;
    if (dirs & AOS_PAD_UP) cursor_move(0, -1);
    if (dirs & AOS_PAD_DOWN) cursor_move(0, 1);
    if (dirs & AOS_PAD_LEFT) cursor_move(-1, 0);
    if (dirs & AOS_PAD_RIGHT) cursor_move(1, 0);
    if (aos_pad_pressed(p, AOS_PAD_L)) digit_step(-1);
    if (aos_pad_pressed(p, AOS_PAD_R)) digit_step(1);
    if (aos_pad_pressed(p, AOS_PAD_A | AOS_PAD_B)) {
        if (!sd_B.cursor) cursor_move(0, 0);
        else if (sd_G.digit) {
            bool notes = sd_G.notes_mode;
            sd_G.notes_mode = aos_pad_pressed(p, AOS_PAD_B);
            sd_game_input(sd_G.digit);
            sd_G.notes_mode = notes;
        }
    }
    sd_board_present();
    sd_ui_refresh_game();
}

bool sd_ui_key(uint32_t key, uint8_t mods)
{
    bool shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0 || (mods & 0x88) != 0;
    bool game = sd_U.screen == SD_SCR_GAME && sd_U.panel_kind == PANEL_NONE && sd_G.active && !sd_U.waiting;
    if (!game) {
        switch (key) {
        case AOS_KEY_UP: sd_U.kb_bits |= AOS_PAD_UP; return true;
        case AOS_KEY_DOWN: sd_U.kb_bits |= AOS_PAD_DOWN; return true;
        case AOS_KEY_LEFT: sd_U.kb_bits |= AOS_PAD_LEFT; return true;
        case AOS_KEY_RIGHT: sd_U.kb_bits |= AOS_PAD_RIGHT; return true;
        case AOS_KEY_ENTER: case ' ': sd_U.kb_bits |= AOS_PAD_A; return true;
        case AOS_KEY_ESC: case AOS_KEY_BACKSPACE: sd_U.kb_bits |= AOS_PAD_B; return true;
        default: break;
        }
        if ((key == 'p' || key == 'P') && sd_U.panel_kind == PANEL_PAUSE) {
            panel_close();
            sd_ui_refresh_game();
            return true;
        }
        return false;
    }
    int n = sd_shape.n;
    if (key >= '1' && key <= '0' + (uint32_t)n) {
        bool notes = sd_G.notes_mode;
        if (shift) sd_G.notes_mode = !notes;
        if (sd_G.sel < 0) cursor_move(0, 0);
        sd_game_input((int)(key - '0'));
        sd_G.notes_mode = notes;
    } else {
        /* shifted digits of a US layout, as notes */
        static const char SH[] = "!@#$%^&*(";
        const char *q = key < 128 ? strchr(SH, (int)key) : NULL;
        if (q && key && (int)(q - SH) < n) {
            bool notes = sd_G.notes_mode;
            sd_G.notes_mode = true;
            if (sd_G.sel < 0) cursor_move(0, 0);
            sd_game_input((int)(q - SH) + 1);
            sd_G.notes_mode = notes;
        } else {
            switch (key) {
            case AOS_KEY_UP: cursor_move(0, -1); break;
            case AOS_KEY_DOWN: cursor_move(0, 1); break;
            case AOS_KEY_LEFT: cursor_move(-1, 0); break;
            case AOS_KEY_RIGHT: cursor_move(1, 0); break;
            case AOS_KEY_BACKSPACE: case AOS_KEY_DEL: case '0': case '.': sd_game_erase(); break;
            case 'n': case 'N': sd_G.notes_mode = !sd_G.notes_mode; break;
            case 'a': case 'A': sd_game_autonotes(); break;
            case 'h': case 'H': case '?': hint_show(); break;
            case 'z': case 'Z': case 'u': case 'U':
                if (ctrl && shift) sd_game_redo();
                else sd_game_undo();
                break;
            case 'y': case 'Y': case 'r': case 'R': sd_game_redo(); break;
            case 'p': case 'P': case AOS_KEY_ESC: pause_show(); break;
            case AOS_KEY_NEXT: digit_step(shift ? -1 : 1); break;
            default: return false;
            }
        }
    }
    sd_board_present();
    sd_ui_refresh_game();
    return true;
}
