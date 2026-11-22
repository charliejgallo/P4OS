/*
 * SOLITARIO - the bars and the panels
 *
 * Upright: a bar on top (the menu, the game's name, time, score and moves)
 * and the buttons at the bottom, under the thumb. Lying down: all of it in a
 * panel down the left side, so the table keeps the whole height.
 *
 * The panels (the menu, the games, the options, the looks, the statistics,
 * the achievements, the win) are one overlay at a time, built when opened
 * and deleted when closed. Every button of a panel goes to aos_pad_menu,
 * so a gamepad walks them with the d-pad and presses with A.
 */
#include "so_app.h"

#include "aos_hal.h"
#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BTN       0x10261A
#define C_BTN_B     0x3E7A55
#define C_PANEL     0x15191D
#define C_PANEL_B   0x3A4148
#define C_GOLD      0xF0CB62
#define C_TEXT      0xF4F1E8
#define C_DIM       0xB4C4BA

/* --- small builders ------------------------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_opa(b, 215, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_BTN_B), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2B5A3D), LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* a toolbar button: a glyph over a word */
static lv_obj_t *tool(lv_obj_t *parent, const char *sym, const char *text, int x, int y, int w, int h,
                      lv_event_cb_t cb, so_app_t *a, bool row)
{
    lv_obj_t *b = button(parent, w, h, cb, a);
    lv_obj_set_pos(b, x, y);
    lv_obj_t *i = label(b, sym, row ? &aos_sym_28 : &aos_sym_44, C_TEXT);
    lv_obj_t *t = label(b, text, &aos_inter_20, C_TEXT);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (row) {
        lv_obj_align(i, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 48, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, w - 54);
    } else {
        lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, w - 12);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    return b;
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* --- the chrome's actions ----------------------------------------------------------------- */

static so_app_t *ud(lv_event_t *e)
{
    return (so_app_t *)lv_event_get_user_data(e);
}

static void menu_cb(lv_event_t *e) { so_ui_panel(ud(e), SO_PANEL_MENU); }
static void new_cb(lv_event_t *e)
{
    so_app_t *a = ud(e);
    so_new_hand(a, a->set.mode, false);
}
static void undo_cb(lv_event_t *e) { so_do_undo(ud(e)); }
static void hint_cb(lv_event_t *e) { so_do_hint(ud(e)); }
static void auto_cb(lv_event_t *e)
{
    so_app_t *a = ud(e);
    if (so_can_autocomplete(&a->g)) a->autoplay = 1;
}

void so_ui_build(so_app_t *a)
{
    so_view_t *v = &a->v;
    int W = v->W, H = v->H;
    a->panel = NULL;
    a->panel_kind = SO_PANEL_NONE;

    if (!v->land) {
        a->btn_menu = button(a->root, 88, 72, menu_cb, a);
        lv_obj_set_pos(a->btn_menu, 16, SO_EDGE_TOP);
        lv_obj_t *ml = label(a->btn_menu, LV_SYMBOL_BARS, &aos_inter_36, C_TEXT);
        lv_obj_center(ml);

        a->lbl_title = label(a->root, "", &aos_inter_24, C_DIM);
        lv_obj_set_pos(a->lbl_title, 120, SO_EDGE_TOP + 2);
        lv_obj_set_width(a->lbl_title, W - 136);
        lv_label_set_long_mode(a->lbl_title, LV_LABEL_LONG_MODE_DOTS);
        int ly = SO_EDGE_TOP + 36;
        a->lbl_time = label(a->root, "", &aos_inter_28, C_TEXT);
        lv_obj_set_pos(a->lbl_time, 120, ly);
        a->lbl_score = label(a->root, "", &aos_inter_28, C_GOLD);
        lv_obj_set_pos(a->lbl_score, 300, ly);
        a->lbl_moves = label(a->root, "", &aos_inter_28, C_TEXT);
        lv_obj_set_pos(a->lbl_moves, 520, ly);

        int bw = (W - 32 - 3 * 12) / 4, bh = 104, by = H - SO_EDGE_BOT - bh;
        a->btn_new = tool(a->root, AOS_SYM_CARDS_PLAYING, _("Nueva"), 16, by, bw, bh, new_cb, a, false);
        a->btn_undo = tool(a->root, AOS_SYM_RESTART, _("Deshacer"), 16 + (bw + 12), by, bw, bh, undo_cb, a, false);
        a->btn_hint = tool(a->root, AOS_SYM_LIGHTBULB_OUTLINE, _("Pista"), 16 + 2 * (bw + 12), by, bw, bh,
                           hint_cb, a, false);
        a->btn_auto = tool(a->root, AOS_SYM_AUTO_FIX, _("Completar"), 16 + 3 * (bw + 12), by, bw, bh, auto_cb,
                           a, false);
    } else {
        lv_obj_t *side = lv_obj_create(a->root);
        lv_obj_remove_style_all(side);
        lv_obj_set_size(side, 200, H);
        lv_obj_set_style_bg_color(side, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(side, 70, 0);
        lv_obj_remove_flag(side, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);

        a->btn_menu = tool(a->root, LV_SYMBOL_BARS, _("Menú"), SO_EDGE_LEFT, SO_EDGE_TOP, 160, 64, menu_cb, a,
                           true);
        lv_obj_set_style_text_font(lv_obj_get_child(a->btn_menu, 0), &aos_inter_28, 0);
        a->lbl_title = label(a->root, "", &aos_inter_20, C_DIM);
        lv_obj_set_pos(a->lbl_title, SO_EDGE_LEFT, SO_EDGE_TOP + 78);
        lv_obj_set_width(a->lbl_title, 164);
        lv_label_set_long_mode(a->lbl_title, LV_LABEL_LONG_MODE_WRAP);
        a->lbl_time = label(a->root, "", &aos_inter_24, C_TEXT);
        lv_obj_set_pos(a->lbl_time, SO_EDGE_LEFT, SO_EDGE_TOP + 140);
        a->lbl_score = label(a->root, "", &aos_inter_24, C_GOLD);
        lv_obj_set_pos(a->lbl_score, SO_EDGE_LEFT, SO_EDGE_TOP + 172);
        a->lbl_moves = label(a->root, "", &aos_inter_24, C_TEXT);
        lv_obj_set_pos(a->lbl_moves, SO_EDGE_LEFT, SO_EDGE_TOP + 204);

        int bh = 72, gap = 10, by = H - 20 - 4 * bh - 3 * gap;
        a->btn_new = tool(a->root, AOS_SYM_CARDS_PLAYING, _("Nueva"), SO_EDGE_LEFT, by, 160, bh, new_cb, a, true);
        a->btn_undo = tool(a->root, AOS_SYM_RESTART, _("Deshacer"), SO_EDGE_LEFT, by + (bh + gap), 160, bh,
                           undo_cb, a, true);
        a->btn_hint = tool(a->root, AOS_SYM_LIGHTBULB_OUTLINE, _("Pista"), SO_EDGE_LEFT, by + 2 * (bh + gap), 160,
                           bh, hint_cb, a, true);
        a->btn_auto = tool(a->root, AOS_SYM_AUTO_FIX, _("Completar"), SO_EDGE_LEFT, by + 3 * (bh + gap), 160, bh,
                           auto_cb, a, true);
    }

    a->lbl_status = label(a->root, "", &aos_inter_24, C_TEXT);
    lv_obj_set_style_bg_color(a->lbl_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->lbl_status, 170, 0);
    lv_obj_set_style_radius(a->lbl_status, 24, 0);
    lv_obj_set_style_pad_hor(a->lbl_status, 24, 0);
    lv_obj_set_style_pad_ver(a->lbl_status, 10, 0);
    lv_obj_set_style_text_align(a->lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(a->lbl_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(a->lbl_status, v->land ? W - 280 : W - 60, 0);
    if (v->land) {
        lv_obj_align(a->lbl_status, LV_ALIGN_BOTTOM_MID, 100, -30);
    } else {
        lv_obj_align(a->lbl_status, LV_ALIGN_BOTTOM_MID, 0, -(SO_EDGE_BOT + 110));
    }
    show(a->lbl_status, a->status_ms > 0);
    if (a->status_ms > 0) lv_label_set_text(a->lbl_status, a->status_txt);
    so_ui_refresh(a);
}

void so_ui_status(so_app_t *a, const char *text, int ms)
{
    snprintf(a->status_txt, sizeof a->status_txt, "%s", text);
    a->status_ms = (int16_t)ms;
    if (a->lbl_status) {
        lv_label_set_text(a->lbl_status, a->status_txt);
        show(a->lbl_status, ms > 0);
        lv_obj_move_to_index(a->lbl_status, -1);
    }
}

void so_ui_refresh(so_app_t *a)
{
    so_game_t *g = &a->g;
    char b[64];
    if (!a->lbl_time) return;
    if (a->daily) {
        snprintf(b, sizeof b, "%s · %s", _(so_mode_name[g->mode]), _("Mano del día"));
    } else {
        snprintf(b, sizeof b, "%s", _(so_mode_name[g->mode]));
    }
    lv_label_set_text(a->lbl_title, b);
    uint32_t s = a->elapsed_ms / 1000;
    snprintf(b, sizeof b, "%s %u:%02u", _("Tiempo"), (unsigned)(s / 60), (unsigned)(s % 60));
    lv_label_set_text(a->lbl_time, b);
    int32_t sc = g->score;
    if (g->scoring == SO_SC_TIMED) {
        sc -= (int32_t)(s / 10) * 2;
        if (sc < 0) sc = 0;
    }
    if (g->scoring == SO_SC_NONE) {
        b[0] = 0;
    } else if (g->scoring == SO_SC_VEGAS) {
        snprintf(b, sizeof b, "%s $%ld", _("Saldo"), (long)sc);
    } else if (g->scoring == SO_SC_VEGAS_CUM) {
        snprintf(b, sizeof b, "%s $%ld", _("Saldo"), (long)(sc + a->data.vegas_bank));
    } else {
        snprintf(b, sizeof b, "%s %ld", _("Puntos"), (long)sc);
    }
    lv_label_set_text(a->lbl_score, b);
    snprintf(b, sizeof b, "%s %lu", _("Movidas"), (unsigned long)g->moves);
    lv_label_set_text(a->lbl_moves, b);
    if (!a->v.land) {
        /* three on a line, spread by what each says */
        lv_obj_update_layout(a->lbl_time);
        int x = 120;
        lv_obj_set_x(a->lbl_time, x);
        x += lv_obj_get_width(a->lbl_time) + 26;
        lv_obj_set_x(a->lbl_score, x);
        if (b[0] && g->scoring != SO_SC_NONE) x += lv_obj_get_width(a->lbl_score) + 26;
        lv_obj_set_x(a->lbl_moves, x);
    }
    show(a->btn_undo, a->set.undo);
    show(a->btn_hint, a->set.hints);
    if (a->btn_undo) {
        if (so_can_undo(g) && !a->finished) lv_obj_remove_state(a->btn_undo, LV_STATE_DISABLED);
        else lv_obj_add_state(a->btn_undo, LV_STATE_DISABLED);
        lv_obj_set_style_opa(a->btn_undo, so_can_undo(g) && !a->finished ? LV_OPA_COVER : 110, 0);
    }
    show(a->btn_auto, so_can_autocomplete(g) && !a->autoplay);
}

/* --- panels --------------------------------------------------------------------------- */

static void panel_close(so_app_t *a)
{
    if (a->panel) {
        lv_obj_delete(a->panel);
        a->panel = NULL;
    }
    a->panel_kind = SO_PANEL_NONE;
    a->npad_items = 0;
    aos_pad_menu_clear(&a->pmenu);
    aos_pad_reset(&a->pad, lv_tick_get());
}

static void pad_add(so_app_t *a, lv_obj_t *b)
{
    if (a->npad_items < AOS_PAD_MENU_MAX) {
        a->pad_items[a->npad_items++] = b;
    }
}

/* the dim cover and the card in the middle, with a title; returns the
 * column the panel's rows go in */
static lv_obj_t *panel_open(so_app_t *a, so_panel_t kind, const char *title)
{
    so_view_t *v = &a->v;
    if (a->panel) {
        lv_obj_delete(a->panel);
    }
    a->panel_kind = kind;
    a->npad_items = 0;
    lv_obj_t *cover = lv_obj_create(a->root);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, v->W, v->H);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cover, 150, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    a->panel = cover;

    int pw = v->land ? 760 : v->W - 48;
    int ph = v->land ? v->H - SO_EDGE_TOP - 24 : v->H - SO_EDGE_TOP - SO_EDGE_BOT - 40;
    lv_obj_t *card = lv_obj_create(cover);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, pw, ph);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, SO_EDGE_TOP + 8);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, 245, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_PANEL_B), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = label(card, title, &aos_inter_36, C_GOLD);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_width(t, pw - 40);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, pw - 16, ph - 90);
    lv_obj_set_pos(col, 8, 80);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_set_style_pad_column(col, 14, 0);
    lv_obj_set_style_pad_top(col, 12, 0);
    lv_obj_set_style_pad_bottom(col, 24, 0);
    lv_obj_set_style_pad_left(col, 12, 0);
    lv_obj_set_style_pad_right(col, 12, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    return col;
}

static int col_w(so_app_t *a)
{
    return (a->v.land ? 760 : a->v.W - 48) - 16 - 24;
}

/* a wide button with a title and, under it, a line of detail */
static lv_obj_t *row(so_app_t *a, lv_obj_t *col, const char *sym, const char *title, const char *sub,
                     int w, lv_event_cb_t cb, intptr_t id)
{
    int h = sub ? 96 : 80;
    lv_obj_t *b = button(col, w, h, cb, a);
    lv_obj_set_user_data(b, (void *)id);
    int x = 18;
    if (sym) {
        lv_obj_t *i = label(b, sym, &aos_sym_44, C_GOLD);
        lv_obj_align(i, LV_ALIGN_LEFT_MID, 6, 0);
        x = 70;
    }
    lv_obj_t *t = label(b, title, &aos_inter_28, C_TEXT);
    lv_obj_set_width(t, w - x - 20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (sub) {
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, x - 18, 6);
        lv_obj_t *s = label(b, sub, &aos_inter_20, C_DIM);
        lv_obj_set_width(s, w - x - 20);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, x - 18, -8);
    } else {
        lv_obj_align(t, LV_ALIGN_LEFT_MID, x - 18, 0);
    }
    pad_add(a, b);
    return b;
}

static intptr_t id_of(lv_event_t *e)
{
    return (intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
}

/* --- the menu ----------------------------------------------------------------------------- */

enum { M_RESUME = 0, M_NEW, M_RESTART, M_GAMES, M_DAILY, M_OPTIONS, M_LOOKS, M_STATS, M_ACH, M_HELP, M_EXIT };

static void menu_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    switch (id_of(e)) {
    case M_RESUME:  panel_close(a); break;
    case M_NEW:     panel_close(a); so_new_hand(a, a->set.mode, false); break;
    case M_RESTART: panel_close(a); so_restart_hand(a); break;
    case M_GAMES:   so_ui_panel(a, SO_PANEL_GAMES); break;
    case M_DAILY: {
        uint32_t d = so_today();
        if (!d) {
            panel_close(a);
            so_ui_status(a, _("La mano del día espera a que la placa tenga hora."), 3000);
            break;
        }
        panel_close(a);
        so_new_hand(a, so_daily_mode(d), true);
        break;
    }
    case M_OPTIONS: so_ui_panel(a, SO_PANEL_OPTIONS); break;
    case M_LOOKS:   so_ui_panel(a, SO_PANEL_LOOKS); break;
    case M_STATS:   so_ui_panel(a, SO_PANEL_STATS); break;
    case M_ACH:     so_ui_panel(a, SO_PANEL_ACH); break;
    case M_HELP:    so_ui_panel(a, SO_PANEL_HELP); break;
    case M_EXIT:    panel_close(a); so_exit(a); break;
    }
}

static void build_menu(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_MENU, _("Solitario"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    row(a, c, AOS_SYM_PLAY, _("Seguir"), NULL, hw, menu_pick, M_RESUME);
    row(a, c, AOS_SYM_CARDS_PLAYING, _("Nueva mano"), _(so_mode_name[a->set.mode]), hw, menu_pick, M_NEW);
    row(a, c, AOS_SYM_RESTART, _("Repetir esta mano"), NULL, hw, menu_pick, M_RESTART);
    row(a, c, AOS_SYM_VIEW_GRID_OUTLINE, _("Elegir juego"), NULL, hw, menu_pick, M_GAMES);
    {
        char sub[80];
        uint32_t d = so_today();
        if (d) {
            snprintf(sub, sizeof sub, "%s%s", _(so_mode_name[so_daily_mode(d)]),
                     a->data.daily_day == d ? _(" · ganada") : "");
        } else {
            snprintf(sub, sizeof sub, "%s", _("Sin hora todavía"));
        }
        row(a, c, AOS_SYM_CALENDAR_TODAY, _("Mano del día"), sub, hw, menu_pick, M_DAILY);
    }
    row(a, c, AOS_SYM_COG, _("Opciones"), NULL, hw, menu_pick, M_OPTIONS);
    row(a, c, AOS_SYM_PALETTE, _("Apariencia"), _("Paños, dorsos y barajas"), hw, menu_pick, M_LOOKS);
    row(a, c, AOS_SYM_CHART_LINE, _("Estadísticas"), NULL, hw, menu_pick, M_STATS);
    row(a, c, AOS_SYM_STAR, _("Logros"), NULL, hw, menu_pick, M_ACH);
    row(a, c, AOS_SYM_INFORMATION_OUTLINE, _("Cómo se juega"), _(so_mode_name[a->g.mode]), hw, menu_pick,
        M_HELP);
    row(a, c, AOS_SYM_CLOSE, _("Salir"), NULL, hw, menu_pick, M_EXIT);
}

/* --- the games ----------------------------------------------------------------------------- */

static const char *const MODE_SUB[SO_NMODES] = {
    N_("Siete columnas, se da vuelta de a una"),
    N_("Siete columnas, se da vuelta de a tres"),
    N_("Dos mazos, todo de un palo"),
    N_("Dos mazos, picas y corazones"),
    N_("Dos mazos, los cuatro palos"),
    N_("Cuatro celdas libres, todo a la vista"),
    N_("Parejas que suman trece"),
    N_("Tres picos, una más o una menos"),
    N_("Siete columnas, una más o una menos"),
    N_("Sin mazo: se mueve cualquier carta"),
    N_("Klondike con la baraja de cuarenta"),
};

static void games_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    int m = (int)id_of(e);
    panel_close(a);
    so_new_hand(a, m, false);
}

static void build_games(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_GAMES, _("Elegir juego"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    for (int m = 0; m < SO_NMODES; m++) {
        char sub[120];
        const so_stat_t *s = &a->data.st[m];
        if (s->played) {
            snprintf(sub, sizeof sub, "%s · %lu/%lu", _(MODE_SUB[m]), (unsigned long)s->won,
                     (unsigned long)s->played);
        } else {
            snprintf(sub, sizeof sub, "%s", _(MODE_SUB[m]));
        }
        lv_obj_t *b = row(a, c, AOS_SYM_CARDS_PLAYING, _(so_mode_name[m]), sub, hw, games_pick, m);
        if (m == a->set.mode) {
            lv_obj_set_style_border_color(b, lv_color_hex(C_GOLD), 0);
        }
    }
}

/* --- options --------------------------------------------------------------------------------- */

enum { O_SCORING = 0, O_LIMITED, O_UNDO, O_HINTS, O_SOLVABLE, O_AUTO, O_WIN, O_SOUND, O_N };

static const char *opt_value(so_app_t *a, int o, char *buf, size_t n)
{
    so_set_t *s = &a->set;
    switch (o) {
    case O_SCORING:  return _(so_scoring_name[s->scoring]);
    case O_LIMITED:  return s->limited ? _("Tres vueltas") : _("Sin límite");
    case O_UNDO:     return s->undo ? _("Sí") : _("No");
    case O_HINTS:    return s->hints ? _("Sí") : _("No");
    case O_SOLVABLE: return s->solvable ? _("Sí") : _("No");
    case O_AUTO:     return s->autocomplete ? _("Sí") : _("No");
    case O_WIN:      return _(so_win_name[s->win_anim]);
    default:         (void)buf; (void)n; return s->sound ? _("Sí") : _("No");
    }
}

static const char *const OPT_NAME[O_N] = {
    N_("Puntaje"), N_("Vueltas al mazo"), N_("Deshacer"), N_("Pistas"),
    N_("Manos con solución"), N_("Completar solo"), N_("Al ganar"), N_("Sonido"),
};

static void opt_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    so_set_t *s = &a->set;
    int o = (int)id_of(e);
    switch (o) {
    case O_SCORING:  s->scoring = (uint8_t)((s->scoring + 1) % SO_NSCORING); break;
    case O_LIMITED:  s->limited ^= 1; break;
    case O_UNDO:     s->undo ^= 1; break;
    case O_HINTS:    s->hints ^= 1; break;
    case O_SOLVABLE: s->solvable ^= 1; break;
    case O_AUTO:     s->autocomplete ^= 1; break;
    case O_WIN:      s->win_anim = (uint8_t)((s->win_anim + 1) % SO_NWINS); break;
    case O_SOUND:    s->sound ^= 1; break;
    }
    so_set_save(s);
    char buf[8];
    lv_obj_t *b = lv_event_get_current_target_obj(e);
    lv_obj_t *val = lv_obj_get_child(b, -1);
    lv_label_set_text(val, opt_value(a, o, buf, sizeof buf));
    so_ui_refresh(a);
}

static void build_options(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_OPTIONS, _("Opciones"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    char buf[8];
    for (int o = 0; o < O_N; o++) {
        lv_obj_t *b = button(c, hw, 80, opt_pick, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)o);
        lv_obj_t *t = label(b, _(OPT_NAME[o]), &aos_inter_28, C_TEXT);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_width(t, hw / 2);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *v = label(b, opt_value(a, o, buf, sizeof buf), &aos_inter_28, C_GOLD);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_width(v, hw / 2 - 40);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        pad_add(a, b);
    }
    lv_obj_t *n = label(c, _("El puntaje y las vueltas al mazo valen desde la próxima mano. Vegas: cada mano "
                             "cuesta una moneda por carta y cada carta que llega a la base paga cinco."),
                        &aos_inter_20, C_DIM);
    lv_obj_set_width(n, w - 20);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
}

/* --- looks: felts, backs and decks, with what unlocks each ------------------------------------------ */

typedef struct {
    so_img_t img;
    lv_image_dsc_t d;
} prev_t;

#define MAX_PREV 24
static prev_t s_prev[MAX_PREV];
static int    s_nprev;

static void prev_free(void)
{
    for (int i = 0; i < s_nprev; i++) {
        lv_image_cache_drop(&s_prev[i].d);
        so_img_free(&s_prev[i].img);
    }
    s_nprev = 0;
}

static lv_image_dsc_t *prev_new(int w, int h, so_img_t **img)
{
    if (s_nprev >= MAX_PREV) return NULL;
    prev_t *p = &s_prev[s_nprev];
    p->img = so_img_new(w, h);
    if (!p->img.px) return NULL;
    s_nprev++;
    memset(&p->d, 0, sizeof p->d);
    p->d.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    p->d.header.w = (uint32_t)w;
    p->d.header.h = (uint32_t)h;
    p->d.header.stride = (uint32_t)w * 4;
    p->d.data = (const uint8_t *)p->img.px;
    p->d.data_size = (uint32_t)w * h * 4;
    *img = &p->img;
    return &p->d;
}

static const char *unlock_text(int kind, int item)
{
    for (int i = 0; i < SO_NACH; i++) {
        if (so_ach[i].kind == kind && so_ach[i].item == item) return _(so_ach[i].desc);
    }
    return "";
}

static void looks_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    int id = (int)id_of(e);
    int kind = id / 100, item = id % 100;
    if (!so_unlocked(a, kind, item)) {
        so_ui_status(a, unlock_text(kind, item), 2500);
        return;
    }
    if (kind == SO_UNL_FELT) a->set.felt = (uint8_t)item;
    if (kind == SO_UNL_BACK) a->set.back = (uint8_t)item;
    if (kind == SO_UNL_DECK) a->set.deck = (uint8_t)item;
    so_set_save(&a->set);
    so_view_set_looks(a);
    so_ui_panel(a, SO_PANEL_LOOKS);         /* again, with the new choice marked */
}

static void tab_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    a->looks_tab = (int)id_of(e);
    so_ui_panel(a, SO_PANEL_LOOKS);
}

static void tile(so_app_t *a, lv_obj_t *c, int kind, int item, const char *name, lv_image_dsc_t *img,
                 bool chosen)
{
    bool open = so_unlocked(a, kind, item);
    int tw = a->v.land ? 160 : 200, th = a->v.land ? 200 : 236;
    lv_obj_t *b = button(c, tw, th, looks_pick, a);
    lv_obj_set_user_data(b, (void *)(intptr_t)(kind * 100 + item));
    if (chosen) {
        lv_obj_set_style_border_color(b, lv_color_hex(C_GOLD), 0);
        lv_obj_set_style_border_width(b, 4, 0);
    }
    if (img) {
        lv_obj_t *i = lv_image_create(b);
        lv_image_set_src(i, img);
        lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
        if (!open) lv_obj_set_style_image_opa(i, 90, 0);
    }
    lv_obj_t *t = label(b, name, &aos_inter_20, open ? C_TEXT : C_DIM);
    lv_obj_set_width(t, tw - 16);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -6);
    if (!open) {
        lv_obj_t *l = label(b, AOS_SYM_LOCK, &aos_sym_44, C_GOLD);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);
    }
    pad_add(a, b);
}

static void build_looks(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_LOOKS, _("Apariencia"));
    int w = col_w(a);
    static const char *const TABS[3] = { N_("Paños"), N_("Dorsos"), N_("Barajas") };
    for (int t = 0; t < 3; t++) {
        lv_obj_t *b = button(c, (w - 40) / 3, 64, tab_pick, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)t);
        lv_obj_t *l = label(b, _(TABS[t]), &aos_inter_24, t == a->looks_tab ? C_GOLD : C_TEXT);
        lv_obj_center(l);
        if (t == a->looks_tab) lv_obj_set_style_border_color(b, lv_color_hex(C_GOLD), 0);
        pad_add(a, b);
    }
    prev_free();
    int tw = a->v.land ? 160 : 200;
    int pw = tw - 16, ph = a->v.land ? 140 : 170;
    if (a->looks_tab == 0) {
        for (int f = 0; f < SO_NFELTS; f++) {
            so_img_t *img = NULL;
            lv_image_dsc_t *d = prev_new(pw, ph, &img);
            if (d) so_felt_draw(img, f, 0, 0);
            tile(a, c, SO_UNL_FELT, f, _(so_felt_name[f]), d, f == a->set.felt);
        }
    } else {
        so_art_t art;
        memset(&art, 0, sizeof art);
        int cw = ph * 5 / 7 - 8;
        so_art_setup(&art, cw, so_card_h_for(cw), SO_DECK_CLASSIC, 0);
        if (a->looks_tab == 1) {
            for (int b = 0; b < SO_NBACKS; b++) {
                so_img_t *img = NULL;
                lv_image_dsc_t *d = prev_new(art.cv_w, art.cv_h, &img);
                if (d) so_back_draw(&art, img, b);
                tile(a, c, SO_UNL_BACK, b, _(so_back_name[b]), d, b == a->set.back);
            }
        } else {
            for (int k = 0; k < SO_NDECKS; k++) {
                so_art_setup(&art, cw, so_card_h_for(cw), k, 0);
                so_img_t *img = NULL;
                lv_image_dsc_t *d = prev_new(art.cv_w, art.cv_h, &img);
                if (d) so_card_draw(&art, img, k == 1 ? 2 : 1, k == 2 ? 6 : 0);
                tile(a, c, SO_UNL_DECK, k, _(so_deck_name[k]), d, k == a->set.deck);
            }
            lv_obj_t *n = label(c, _("La Española usa siempre la baraja de cuarenta."), &aos_inter_20, C_DIM);
            lv_obj_set_width(n, w - 20);
            lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
        }
        so_art_free(&art);
    }
}

/* --- statistics -------------------------------------------------------------------------------- */

static void build_stats(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_STATS, _("Estadísticas"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    char b[200];
    for (int m = 0; m < SO_NMODES; m++) {
        const so_stat_t *s = &a->data.st[m];
        lv_obj_t *box = lv_obj_create(c);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, hw, 124);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x1F262B), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 16, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *t = label(box, _(so_mode_name[m]), &aos_inter_28, C_GOLD);
        lv_obj_set_pos(t, 16, 8);
        unsigned pct = s->played ? (unsigned)(s->won * 100 / s->played) : 0;
        snprintf(b, sizeof b, "%s %lu · %s %lu (%u%%) · %s %lu/%lu", _("Jugadas"), (unsigned long)s->played,
                 _("Ganadas"), (unsigned long)s->won, pct, _("Racha"), (unsigned long)s->streak,
                 (unsigned long)s->best_streak);
        lv_obj_t *l1 = label(box, b, &aos_inter_20, C_TEXT);
        lv_obj_set_pos(l1, 16, 48);
        lv_obj_set_width(l1, hw - 32);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_MODE_DOTS);
        char bt[24] = "-", bs[24] = "-";
        if (s->best_time) snprintf(bt, sizeof bt, "%lu:%02lu", (unsigned long)(s->best_time / 60),
                                   (unsigned long)(s->best_time % 60));
        if (s->has_score) snprintf(bs, sizeof bs, "%ld", (long)s->best_score);
        snprintf(b, sizeof b, "%s %s · %s %s", _("Mejor tiempo"), bt, _("Mejor puntaje"), bs);
        lv_obj_t *l2 = label(box, b, &aos_inter_20, C_DIM);
        lv_obj_set_pos(l2, 16, 80);
        lv_obj_set_width(l2, hw - 32);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
    }
    snprintf(b, sizeof b, "%s $%ld · %s %lu", _("Saldo de Vegas"), (long)a->data.vegas_bank,
             _("Manos del día ganadas"), (unsigned long)a->data.daily_streak);
    lv_obj_t *l = label(c, b, &aos_inter_24, C_TEXT);
    lv_obj_set_width(l, w - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *ok = row(a, c, AOS_SYM_CHECK, _("Listo"), NULL, hw, menu_pick, M_RESUME);
    (void)ok;
}

/* --- achievements ------------------------------------------------------------------------------- */

static const char *unlock_name(int kind, int item, char *buf, size_t n)
{
    switch (kind) {
    case SO_UNL_FELT: snprintf(buf, n, "%s: %s", _("Paño"), _(so_felt_name[item])); break;
    case SO_UNL_BACK: snprintf(buf, n, "%s: %s", _("Dorso"), _(so_back_name[item])); break;
    case SO_UNL_DECK: snprintf(buf, n, "%s: %s", _("Baraja"), _(so_deck_name[item])); break;
    default:          buf[0] = 0; break;
    }
    return buf;
}

static void build_ach(so_app_t *a)
{
    int got = 0;
    for (int i = 0; i < SO_NACH; i++) got += (a->data.ach >> i) & 1;
    char title[64];
    snprintf(title, sizeof title, "%s  %d/%d", _("Logros"), got, SO_NACH);
    lv_obj_t *c = panel_open(a, SO_PANEL_ACH, title);
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    for (int i = 0; i < SO_NACH; i++) {
        bool on = (a->data.ach >> i) & 1;
        char sub[160], un[80];
        unlock_name(so_ach[i].kind, so_ach[i].item, un, sizeof un);
        snprintf(sub, sizeof sub, "%s%s%s", _(so_ach[i].desc), un[0] ? " · " : "", un);
        lv_obj_t *b = row(a, c, on ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE, _(so_ach[i].name), sub, hw, NULL, i);
        if (!on) lv_obj_set_style_opa(b, 160, 0);
    }
}

/* --- help ------------------------------------------------------------------------------------- */

static const char *const HELP[SO_NMODES] = {
    N_("Llevá las cuatro bases del as al rey, cada una de un palo. En las columnas se baja de a uno "
       "alternando rojo y negro, y una columna vacía sólo toma un rey. Tocá el mazo para dar vuelta una carta."),
    N_("Como Klondike, pero el mazo se da vuelta de a tres cartas y sólo se juega la de arriba."),
    N_("Armá en las columnas escaleras del rey al as de un mismo palo: completas, se van solas. Se puede "
       "bajar sobre cualquier palo, pero sólo se mueve junto lo que es del mismo palo. El mazo reparte una "
       "carta en cada columna, si ninguna está vacía."),
    N_("Spider con dos palos: picas y corazones."),
    N_("Spider con los cuatro palos: el más difícil."),
    N_("Todas las cartas a la vista. Las cuatro celdas guardan una carta cada una. En las columnas se baja "
       "alternando colores; se mueven tantas cartas juntas como celdas y columnas libres alcancen."),
    N_("Sacá parejas de cartas libres que sumen trece (la J vale 11, la Q 12; el rey sale solo). Una carta "
       "está libre si no tiene nada encima. El mazo da vuelta de a una."),
    N_("Pasá a la pila de abajo cartas libres que sean una más o una menos que la de arriba (del rey se "
       "sigue al as). Despejá los tres picos. Si no hay jugada, tocá el mazo."),
    N_("Pasá a la pila cartas de abajo de las columnas que sean una más o una menos que la de arriba, sin dar "
       "la vuelta del rey al as. Despejá las siete columnas."),
    N_("Como Klondike, sin mazo: todo está repartido y se puede mover cualquier carta dada vuelta, con todo "
       "lo que tiene encima, sobre una de un número más y del otro color."),
    N_("Klondike con la baraja española de cuarenta: después del 7 viene la sota. En las columnas se alternan "
       "oros y copas con espadas y bastos, y una columna vacía sólo toma un rey."),
};

static void build_help(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_HELP, _(so_mode_name[a->g.mode]));
    int w = col_w(a);
    lv_obj_t *t = label(c, _(HELP[a->g.mode]), &aos_inter_28, C_TEXT);
    lv_obj_set_width(t, w - 20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *t2 = label(c, _("Arrastrá una carta o una pila, o tocala para que vaya sola al mejor lugar; "
                              "dos toques la mandan a la base. Con joystick: la cruz recorre las pilas, A "
                              "toma y suelta, B cancela, L deshace, R da una pista."),
                         &aos_inter_24, C_DIM);
    lv_obj_set_width(t2, w - 20);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_MODE_WRAP);
    row(a, c, AOS_SYM_CHECK, _("Listo"), NULL, a->v.land ? (w - 14) / 2 : w, menu_pick, M_RESUME);
}

/* --- the end of a hand ------------------------------------------------------------------------- */

static void won_pick(lv_event_t *e)
{
    so_app_t *a = ud(e);
    int id = (int)id_of(e);
    panel_close(a);
    if (id == 0) so_new_hand(a, a->set.mode, false);
    else if (id == 1) so_ui_panel(a, SO_PANEL_GAMES);
    else if (id == 2) so_do_undo(a);
}

uint32_t so_won_new_bits;

static void build_won(so_app_t *a)
{
    so_game_t *g = &a->g;
    lv_obj_t *c = panel_open(a, SO_PANEL_WON, _("¡Ganaste!"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    char b[160];
    uint32_t s = a->elapsed_ms / 1000;
    snprintf(b, sizeof b, "%s%s\n%s %u:%02u · %s %lu", _(so_mode_name[g->mode]),
             a->daily ? _(" · Mano del día") : "", _("Tiempo"), (unsigned)(s / 60), (unsigned)(s % 60),
             _("Movidas"), (unsigned long)g->moves);
    lv_obj_t *l = label(c, b, &aos_inter_28, C_TEXT);
    lv_obj_set_width(l, w - 20);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (g->scoring != SO_SC_NONE) {
        int32_t sc = g->score;
        if (g->scoring == SO_SC_TIMED) {
            sc -= (int32_t)(s / 10) * 2;
            if (s >= 30) sc += (int32_t)(700000 / s);
            if (sc < 0) sc = 0;
        }
        if (g->scoring == SO_SC_VEGAS || g->scoring == SO_SC_VEGAS_CUM) {
            snprintf(b, sizeof b, "%s $%ld", _("Saldo"), (long)sc);
        } else {
            snprintf(b, sizeof b, "%s %ld", _("Puntos"), (long)sc);
        }
        lv_obj_t *p = label(c, b, &aos_inter_36, C_GOLD);
        lv_obj_set_width(p, w - 20);
        lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    }
    const so_stat_t *st = &a->data.st[g->mode];
    snprintf(b, sizeof b, "%s %lu · %s %lu", _("Ganadas"), (unsigned long)st->won, _("Racha"),
             (unsigned long)st->streak);
    lv_obj_t *p2 = label(c, b, &aos_inter_24, C_DIM);
    lv_obj_set_width(p2, w - 20);
    lv_obj_set_style_text_align(p2, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < SO_NACH; i++) {
        if (!((so_won_new_bits >> i) & 1)) continue;
        char un[80], sub[160];
        unlock_name(so_ach[i].kind, so_ach[i].item, un, sizeof un);
        snprintf(sub, sizeof sub, "%s%s%s", _(so_ach[i].desc), un[0] ? " · " : "", un);
        lv_obj_t *r = row(a, c, AOS_SYM_STAR, _(so_ach[i].name), sub, hw, NULL, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(C_GOLD), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        a->npad_items--;
    }
    row(a, c, AOS_SYM_CARDS_PLAYING, _("Otra mano"), NULL, hw, won_pick, 0);
    row(a, c, AOS_SYM_VIEW_GRID_OUTLINE, _("Elegir juego"), NULL, hw, won_pick, 1);
}

static void build_stuck(so_app_t *a)
{
    lv_obj_t *c = panel_open(a, SO_PANEL_STUCK, _("No quedan jugadas"));
    int w = col_w(a);
    int hw = a->v.land ? (w - 14) / 2 : w;
    lv_obj_t *l = label(c, _("Esta mano ya no tiene movimientos."), &aos_inter_28, C_TEXT);
    lv_obj_set_width(l, w - 20);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (a->set.undo && so_can_undo(&a->g)) {
        row(a, c, AOS_SYM_RESTART, _("Deshacer"), NULL, hw, won_pick, 2);
    }
    row(a, c, AOS_SYM_CARDS_PLAYING, _("Otra mano"), NULL, hw, won_pick, 0);
    row(a, c, AOS_SYM_VIEW_GRID_OUTLINE, _("Elegir juego"), NULL, hw, won_pick, 1);
}

void so_ui_panel(so_app_t *a, so_panel_t p)
{
    if (p == SO_PANEL_NONE) {
        panel_close(a);
        prev_free();
        return;
    }
    if (p != SO_PANEL_LOOKS) prev_free();
    switch (p) {
    case SO_PANEL_MENU:    build_menu(a); break;
    case SO_PANEL_GAMES:   build_games(a); break;
    case SO_PANEL_OPTIONS: build_options(a); break;
    case SO_PANEL_LOOKS:   build_looks(a); break;
    case SO_PANEL_STATS:   build_stats(a); break;
    case SO_PANEL_ACH:     build_ach(a); break;
    case SO_PANEL_WON:     build_won(a); break;
    case SO_PANEL_STUCK:   build_stuck(a); break;
    case SO_PANEL_HELP:    build_help(a); break;
    default: break;
    }
    aos_pad_menu_set(&a->pmenu, a->pad_items, a->npad_items, 0);
    aos_pad_reset(&a->pad, lv_tick_get());
}

void so_ui_frame(so_app_t *a, int dt)
{
    if (a->status_ms > 0) {
        a->status_ms = (int16_t)(a->status_ms - dt);
        if (a->status_ms <= 0) show(a->lbl_status, false);
    }
}

/* the gamepad inside a panel: the outline over its buttons; B closes */
void so_ui_pad(so_app_t *a)
{
    if (!a->panel) return;
    if (aos_pad_menu_step(&a->pmenu, &a->pad)) {
        /* keep the outlined one in sight in a scrolling column */
        lv_obj_t *sel = aos_pad_menu_selected(&a->pmenu);
        if (sel) lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
        return;
    }
    if (aos_pad_pressed(&a->pad, AOS_PAD_B | AOS_PAD_START)) {
        if (a->panel_kind == SO_PANEL_WON || a->panel_kind == SO_PANEL_STUCK) return;
        so_ui_panel(a, a->panel_kind == SO_PANEL_MENU ? SO_PANEL_NONE : SO_PANEL_MENU);
    }
}
