/*
 * Juegos de mesa - what LVGL draws over the table: a card for each player
 * (its colour, who plays it, its clock, a line of the game's), the
 * buttons, a line of status, the score sheet, the coordinates, and the
 * panels (the pause, the end of a game, the choice of a promotion).
 *
 * Upright the cards are above and below the board and the buttons a row
 * at the bottom; lying down the cards are in the left column and the
 * buttons and the sheet in the right one. Face to face the far player's
 * card and buttons are turned half round, for whoever sits across.
 */
#include "jm_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define P (&jm->play)
#define CARD_H 112
#define BTN 84
#define COL 240                 /* lying down: the side columns' width */

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- small things --------------------------------------------------------------------- */

static void box_style(lv_obj_t *o, uint32_t bg, int opa, int radius)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, C(bg), 0);
    lv_obj_set_style_bg_opa(o, (lv_opa_t)opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN, BTN);
    lv_obj_set_style_radius(b, BTN / 2, 0);
    lv_obj_set_style_bg_color(b, C(0x1C1612), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, C(0x4A3A2C), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C(0xC8A878), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, &aos_sym_28, 0);
    lv_obj_set_style_text_color(l, C(0xF4E8D4), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void ev_menu(lv_event_t *e)
{
    (void)e;
    jm_hud_pause();
}

static void ev_undo(lv_event_t *e)
{
    (void)e;
    jm_play_undo();
}

static void ev_redo(lv_event_t *e)
{
    (void)e;
    jm_play_redo();
}

static void ev_hint(lv_event_t *e)
{
    (void)e;
    jm_play_hint();
}

static void ev_roll(lv_event_t *e)
{
    (void)e;
    jm_play_roll();
}

static void ev_sheet(lv_event_t *e)
{
    (void)e;
    jm_hud_sheet_toggle();
}

static void ev_table(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    (void)e;
    jm_play_tap(pt.x, pt.y);
}

/* ---- the cards ------------------------------------------------------------------------- */

static int face_to_face(void)
{
    int people = 0;
    for (int i = 0; i < P->np; i++) people += P->human[i] ? 1 : 0;
    return people >= 2 && P->two == TWO_FACE && P->np == 2;
}

static void turn_round(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    lv_obj_set_style_transform_pivot_x(o, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(o, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(o, 1800, 0);
}

static void card_build(int slot, int player, int x, int y, int w, bool turned)
{
    lv_obj_t *c = lv_obj_create(jm->hud);
    box_style(c, 0x140F0B, LV_OPA_70, 22);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, CARD_H);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_border_color(c, C(0xC8A878), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    uint32_t rgb = 0xFFFFFF;
    const char *side = P->g->side(P->opts, player, &rgb);
    lv_obj_t *dot = lv_obj_create(c);
    box_style(dot, rgb, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(dot, 34, 34);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, C(0xC8A878), 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *nm = lv_label_create(c);
    lv_obj_set_style_text_font(nm, aos_font_body, 0);
    lv_obj_set_style_text_color(nm, C(0xF4E8D4), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    bool narrow = w < 360;     /* lying down: the clock goes under the name */
    lv_obj_set_width(nm, w - 80 - (P->clock.mode != CLK_NONE && !narrow ? 130 : 0));
    lv_obj_set_height(nm, lv_font_get_line_height(aos_font_body));
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 66, 16);
    char buf[96];
    if (P->human[player]) snprintf(buf, sizeof buf, "%s", _(side));
    else snprintf(buf, sizeof buf, "%s · %s %d", _(side), _("Placa"), P->level[player]);
    lv_label_set_text(nm, buf);
    lv_obj_t *in = lv_label_create(c);
    lv_obj_set_style_text_font(in, aos_font_small, 0);
    lv_obj_set_style_text_color(in, C(0xB8A890), 0);
    lv_label_set_long_mode(in, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(in, w - 80 - (P->clock.mode != CLK_NONE ? 130 : 0));
    lv_obj_set_height(in, lv_font_get_line_height(aos_font_small));
    lv_obj_align(in, LV_ALIGN_BOTTOM_LEFT, 66, -14);
    lv_label_set_text(in, "");
    lv_obj_t *ck = NULL;
    if (P->clock.mode != CLK_NONE) {
        ck = lv_label_create(c);
        lv_obj_set_style_text_font(ck, narrow ? aos_font_body : aos_font_title, 0);
        lv_obj_set_style_text_color(ck, C(0xF4E8D4), 0);
        if (narrow) lv_obj_align(ck, LV_ALIGN_BOTTOM_RIGHT, -18, -12);
        else lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_label_set_text(ck, "");
    }
    if (slot < 2) {
        jm->card[slot] = c;
        jm->card_name[slot] = nm;
        jm->card_info[slot] = in;
        jm->card_clock[slot] = ck;
        jm->card_dot[slot] = dot;
    }
    lv_obj_set_user_data(c, (void *)(intptr_t)player);
    if (turned) turn_round(c);
}

/* ---- the buttons ------------------------------------------------------------------------ */

static lv_obj_t *s_cards4[MESA_MAX_PLAYERS];

static void bar_build(void)
{
    int W = jm->W, H = jm->H;
    bool pgn = (P->g->flags & MESA_GF_PGN) != 0;
    lv_obj_t *bar = lv_obj_create(jm->hud);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(bar, jm->portrait ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (jm->portrait) {
        lv_obj_set_size(bar, W - 32, BTN + 16);
        lv_obj_set_pos(bar, 16, H - BTN - 28);
    } else {
        lv_obj_set_size(bar, COL, 2 * BTN + 30);
        lv_obj_set_style_pad_row(bar, 16, 0);
        lv_obj_set_pos(bar, W - COL - 16, H - 2 * BTN - 46);
    }
    jm->bar = bar;
    jm->btn_menu = icon_btn(bar, AOS_SYM_DOTS_HORIZONTAL, ev_menu);
    jm->btn_undo = icon_btn(bar, AOS_SYM_CHEVRON_LEFT, ev_undo);
    jm->btn_redo = icon_btn(bar, AOS_SYM_CHEVRON_RIGHT, ev_redo);
    jm->btn_hint = P->g->ai_move ? icon_btn(bar, AOS_SYM_LIGHTBULB_OUTLINE, ev_hint) : NULL;
    jm->btn_roll = (P->g->flags & MESA_GF_DICE) ? icon_btn(bar, AOS_SYM_DICE_5, ev_roll) : NULL;
    jm->btn_sheet = pgn && jm->portrait ? icon_btn(bar, AOS_SYM_FILE_DOCUMENT_OUTLINE, ev_sheet) : NULL;
    jm->btn_menu2 = jm->btn_undo2 = NULL;
    if (face_to_face()) {
        /* the far player's own menu and undo, turned */
        lv_obj_t *b2 = lv_obj_create(jm->hud);
        lv_obj_remove_style_all(b2);
        lv_obj_remove_flag(b2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(b2, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(b2, 20, 0);
        lv_obj_set_size(b2, 2 * BTN + 40, BTN + 10);
        if (jm->portrait) lv_obj_set_pos(b2, 16, 16 + CARD_H + 10);
        else lv_obj_set_pos(b2, 16, 16 + CARD_H + 10);
        jm->btn_menu2 = icon_btn(b2, AOS_SYM_DOTS_HORIZONTAL, ev_menu);
        jm->btn_undo2 = icon_btn(b2, AOS_SYM_CHEVRON_LEFT, ev_undo);
        turn_round(b2);
    }
}

/* ---- the score sheet ---------------------------------------------------------------------- */

/* "Cf3" from "Nf3": the pieces' letters of the language */
static void localize(const char *san, char *out, size_t n)
{
    const char *L = _("RDTAC");     /* king, queen, rook, bishop, knight */
    size_t k = 0;
    for (const char *s = san; *s && k + 1 < n; s++) {
        const char *E = "KQRBN";
        const char *q = strchr(E, *s);
        if (q && *s && strlen(L) == 5) out[k++] = L[q - E];
        else out[k++] = *s;
    }
    out[k] = 0;
}

void jm_hud_sheet_refresh(void)
{
    if (!jm->sheet_list || !(P->g->flags & MESA_GF_PGN)) return;
    lv_obj_t *l = jm->sheet_list;
    lv_obj_clean(l);
    /* the moves again, from the start, each written before it is made */
    P->g->init(P->tmp, P->opts, P->seed);
    if (P->fen[0] && P->g->setup) P->g->setup(P->tmp, P->opts, P->fen);
    char line[64], a[24], b[24];
    int first = P->g->turn(P->tmp);
    a[0] = b[0] = 0;
    int moveno = 1;
    if (first == 1) {
        snprintf(a, sizeof a, "...");
    }
    for (int i = 0; i < P->n; i++) {
        const jm_hist_t *h = &P->hist[i];
        if (h->roll) {
            P->g->roll(P->tmp, h->rnd);
            continue;
        }
        char san[24], loc[24];
        san[0] = 0;
        if (P->g->notation) P->g->notation(P->tmp, &h->m, san, sizeof san);
        localize(san, loc, sizeof loc);
        int who = P->g->turn(P->tmp);
        P->g->apply(P->tmp, &h->m);
        if (who == 0) {
            snprintf(a, sizeof a, "%s", loc);
        } else {
            snprintf(b, sizeof b, "%s", loc);
            snprintf(line, sizeof line, "%d.  %s   %s", moveno++, a, b);
            lv_obj_t *r = lv_label_create(l);
            lv_obj_set_style_text_font(r, aos_font_body, 0);
            lv_obj_set_style_text_color(r, C(0xF4E8D4), 0);
            lv_label_set_text(r, line);
            a[0] = b[0] = 0;
        }
    }
    if (a[0]) {
        snprintf(line, sizeof line, "%d.  %s", moveno, a);
        lv_obj_t *r = lv_label_create(l);
        lv_obj_set_style_text_font(r, aos_font_body, 0);
        lv_obj_set_style_text_color(r, C(0xF4E8D4), 0);
        lv_label_set_text(r, line);
    }
    lv_obj_update_layout(l);
    lv_obj_scroll_to_y(l, LV_COORD_MAX, LV_ANIM_OFF);
}

static void sheet_build(void)
{
    jm->sheet = jm->sheet_list = NULL;
    if (!(P->g->flags & MESA_GF_PGN)) return;
    lv_obj_t *s = lv_obj_create(jm->hud);
    box_style(s, 0x140F0B, LV_OPA_80, 22);
    lv_obj_set_style_pad_all(s, 16, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    if (jm->portrait) {
        lv_obj_set_size(s, jm->W - 32, 560);
        lv_obj_set_pos(s, 16, jm->H - BTN - 44 - 560);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_size(s, COL, jm->H - 2 * BTN - 46 - 32);
        lv_obj_set_pos(s, jm->W - COL - 16, 16);
    }
    lv_obj_t *t = lv_label_create(s);
    lv_obj_set_style_text_font(t, aos_font_small, 0);
    lv_obj_set_style_text_color(t, C(0xC8A878), 0);
    lv_label_set_text(t, _("Planilla"));
    lv_obj_t *l = lv_obj_create(s);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_top(l, 40, 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 6, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    jm->sheet = s;
    jm->sheet_list = l;
    jm_hud_sheet_refresh();
}

void jm_hud_sheet_toggle(void)
{
    if (!jm->sheet) return;
    if (lv_obj_has_flag(jm->sheet, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(jm->sheet, LV_OBJ_FLAG_HIDDEN);
        jm_hud_sheet_refresh();
    } else {
        lv_obj_add_flag(jm->sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- coordinates ------------------------------------------------------------------------------- */

void jm_hud_coords(void)
{
    for (int i = 0; i < jm->ncoord; i++) {
        if (jm->coord[i]) lv_obj_delete(jm->coord[i]);
        jm->coord[i] = NULL;
    }
    jm->ncoord = 0;
    if (!jm->prefs.coords || !(P->g->flags & MESA_GF_GRID) || !jm->hud) return;
    float bw = jm->view.bw, bh = jm->view.bh;
    bool turned = (jm->view.rot & 3) == 2;
    int nf = (int)bw, nr = (int)bh;
    for (int i = 0; i < nf + nr && jm->ncoord < 40; i++) {
        float u, v, x, y;
        char t[16];
        if (i < nf) {
            u = (float)i + 0.5f;
            v = turned ? bh + 0.28f : -0.28f;
            snprintf(t, sizeof t, "%c", 'a' + i);
        } else {
            int r = i - nf;
            u = turned ? bw + 0.3f : -0.3f;
            v = (float)r + 0.5f;
            snprintf(t, sizeof t, "%d", r + 1);
        }
        jm_view_board_xy(u, v, 0, &x, &y);
        lv_obj_t *l = lv_label_create(jm->hud);
        lv_obj_set_style_text_font(l, aos_font_tiny, 0);
        lv_obj_set_style_text_color(l, C(0xF4E8D4), 0);
        lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
        lv_label_set_text(l, t);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(l, (int)x - 5, (int)y - 10);
        lv_obj_move_to_index(l, 0);
        jm->coord[jm->ncoord++] = l;
    }
}

/* ---- building and refreshing --------------------------------------------------------------------- */

void jm_hud_free(void)
{
    jm_hud_panel_close();
    if (jm->hud) lv_obj_delete(jm->hud);
    jm->hud = NULL;
    memset(jm->card, 0, sizeof jm->card);
    memset(jm->card_name, 0, sizeof jm->card_name);
    memset(jm->card_clock, 0, sizeof jm->card_clock);
    memset(jm->card_info, 0, sizeof jm->card_info);
    memset(jm->card_dot, 0, sizeof jm->card_dot);
    memset(s_cards4, 0, sizeof s_cards4);
    jm->status = jm->bar = jm->sheet = jm->sheet_list = NULL;
    jm->btn_undo = jm->btn_redo = jm->btn_menu = jm->btn_sheet = jm->btn_roll = jm->btn_hint = NULL;
    jm->btn_menu2 = jm->btn_undo2 = NULL;
    jm->ncoord = 0;
    memset(jm->coord, 0, sizeof jm->coord);
    aos_pad_menu_clear(&jm->menu);
}

void jm_hud_build(void)
{
    jm_hud_free();
    if (!P->g) return;
    lv_obj_t *h = lv_obj_create(jm->root);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, jm->W, jm->H);
    lv_obj_set_pos(h, 0, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h, ev_table, LV_EVENT_PRESSED, NULL);
    jm->hud = h;
    int W = jm->W, H = jm->H;
    if (P->np <= 2) {
        int top = P->np == 2 ? 1 - P->bottom : -1;
        if (jm->portrait) {
            card_build(0, P->bottom, 16, H - BTN - 44 - CARD_H, W - 32, false);
            if (top >= 0) card_build(1, top, 16, 16, W - 32, face_to_face());
        } else {
            card_build(0, P->bottom, 16, H - 16 - CARD_H, COL, false);
            if (top >= 0) card_build(1, top, 16, 16, COL, face_to_face());
        }
    } else {
        /* one in each corner, the way round the board the players sit
         * (games of more than two put the first at the bottom left, then
         * the bottom right, the top right, the top left) */
        int cw = jm->portrait ? (W - 48) / 2 : COL;
        for (int p = 0; p < P->np && p < MESA_MAX_PLAYERS; p++) {
            bool right = p == 1 || p == 2, low = p == 0 || p == 1;
            int cx, cy;
            if (jm->portrait) {
                cx = right ? W - 16 - cw : 16;
                cy = low ? H - BTN - 44 - CARD_H : 16;
            } else {
                cx = right ? W - COL - 16 : 16;
                cy = low ? H - 16 - CARD_H : 16;
            }
            card_build(p < 2 ? p : 2, p, cx, cy, cw, false);
            s_cards4[p] = lv_obj_get_child(h, -1);
            if (p < 2) jm->card[p] = s_cards4[p];
        }
    }
    bar_build();
    if (P->np > 2 && !jm->portrait) lv_obj_set_pos(jm->bar, W - COL - 16, H / 2 - BTN - 15);
    sheet_build();
    lv_obj_t *st = lv_label_create(h);
    lv_obj_set_style_text_font(st, aos_font_body, 0);
    lv_obj_set_style_text_color(st, C(0xFFF4E0), 0);
    lv_obj_set_style_bg_color(st, C(0x140F0B), 0);
    lv_obj_set_style_bg_opa(st, LV_OPA_80, 0);
    lv_obj_set_style_radius(st, 26, 0);
    lv_obj_set_style_pad_hor(st, 26, 0);
    lv_obj_set_style_pad_ver(st, 10, 0);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
    if (jm->portrait) lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 16 + CARD_H + 14);
    else lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_flag(st, LV_OBJ_FLAG_HIDDEN);
    jm->status = st;
    jm_hud_coords();
    jm_hud_refresh();
}

void jm_hud_status(const char *text, uint32_t ms)
{
    if (!jm->status) return;
    snprintf(jm->status_text, sizeof jm->status_text, "%s", text ? text : "");
    lv_label_set_text(jm->status, jm->status_text);
    if (text && *text) lv_obj_remove_flag(jm->status, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(jm->status, LV_OBJ_FLAG_HIDDEN);
    jm->status_until = ms ? jm_now() + ms : 0;
}

static lv_obj_t *card_of(int player, lv_obj_t **name, lv_obj_t **info, lv_obj_t **clock)
{
    if (P->np > 2) {
        lv_obj_t *c = s_cards4[player];
        if (!c) return NULL;
        if (name) *name = lv_obj_get_child(c, 1);
        if (info) *info = lv_obj_get_child(c, 2);
        if (clock) *clock = NULL;
        return c;
    }
    for (int s = 0; s < 2; s++) {
        if (jm->card[s] && (int)(intptr_t)lv_obj_get_user_data(jm->card[s]) == player) {
            if (name) *name = jm->card_name[s];
            if (info) *info = jm->card_info[s];
            if (clock) *clock = jm->card_clock[s];
            return jm->card[s];
        }
    }
    return NULL;
}

void jm_hud_clock(void)
{
    if (P->clock.mode == CLK_NONE) return;
    for (int p = 0; p < 2; p++) {
        lv_obj_t *ck = NULL;
        if (!card_of(p, NULL, NULL, &ck) || !ck) continue;
        char b[16];
        jm_clock_text(P->clock.left[p], b, sizeof b);
        lv_label_set_text(ck, b);
        bool low = P->clock.left[p] < 20000;
        lv_obj_set_style_text_color(ck, low ? C(0xFF6A50) : C(0xF4E8D4), 0);
    }
}

void jm_hud_refresh(void)
{
    if (!jm->hud || !P->g) return;
    int t = jm_play_turn();
    for (int p = 0; p < P->np; p++) {
        lv_obj_t *info = NULL;
        lv_obj_t *c = card_of(p, NULL, &info, NULL);
        if (!c) continue;
        bool on = p == t;
        lv_obj_set_style_border_opa(c, on ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_set_style_border_color(c, on ? AOS_C_ACCENT : C(0xC8A878), 0);
        char b[64];
        b[0] = 0;
        if (on && !P->human[p] && P->ai_busy) snprintf(b, sizeof b, "%s", _("Piensa..."));
        else if (P->g->info && P->g->info(P->st, p, b, sizeof b)) {
            char tr[64];
            snprintf(tr, sizeof tr, "%s", _(b));
            snprintf(b, sizeof b, "%s", tr);
        }
        if (info) lv_label_set_text(info, b);
    }
    bool person = jm_play_person_to_move();
    if (jm->btn_undo) {
        if (jm_play_can_undo()) lv_obj_remove_state(jm->btn_undo, LV_STATE_DISABLED);
        else lv_obj_add_state(jm->btn_undo, LV_STATE_DISABLED);
    }
    if (jm->btn_redo) {
        if (jm_play_can_redo()) lv_obj_remove_state(jm->btn_redo, LV_STATE_DISABLED);
        else lv_obj_add_state(jm->btn_redo, LV_STATE_DISABLED);
    }
    if (jm->btn_hint) {
        if (person && !P->over) lv_obj_remove_state(jm->btn_hint, LV_STATE_DISABLED);
        else lv_obj_add_state(jm->btn_hint, LV_STATE_DISABLED);
    }
    if (jm->btn_roll) {
        bool roll = person && P->g->need_roll && P->g->need_roll(P->st) > 0 && !P->rolling;
        if (roll) lv_obj_remove_state(jm->btn_roll, LV_STATE_DISABLED);
        else lv_obj_add_state(jm->btn_roll, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(jm->btn_roll, roll ? AOS_C_ACCENT : C(0x1C1612), 0);
    }
    /* the line of status: the game's, else whose turn */
    if (!jm->status_until || (int32_t)(jm_now() - jm->status_until) >= 0) {
        const char *s = P->g->status ? P->g->status(P->st) : NULL;
        if (!P->over && s) jm_hud_status(_(s), 0);
        else if (!P->over && person && P->g->need_roll && P->g->need_roll(P->st) > 0) jm_hud_status(_("Tirá los dados"), 0);
        else jm_hud_status(NULL, 0);
        jm->status_until = 0;
    }
    jm_hud_clock();
}

/* ---- panels -------------------------------------------------------------------------------------- */

void jm_hud_panel_close(void)
{
    if (!jm->panel) return;
    aos_pad_menu_clear(&jm->menu);
    lv_obj_delete(jm->panel);
    jm->panel = NULL;
    jm->panel_kind = PANEL_NONE;
    aos_pad_reset(&jm->pad, lv_tick_get());
    if (P->clock.mode != CLK_NONE) P->clock.last = jm_now();
}

static lv_obj_t *s_pbox;

static lv_obj_t *panel_box(int kind, const char *title, const char *sub)
{
    jm_hud_panel_close();
    lv_obj_t *dim = lv_obj_create(jm->root);
    box_style(dim, 0x000000, LV_OPA_50, 0);
    lv_obj_set_size(dim, jm->W, jm->H);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *p = lv_obj_create(dim);
    box_style(p, 0x1A140F, LV_OPA_COVER, 32);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_border_color(p, C(0xC8A878), 0);
    lv_obj_set_style_border_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(p, 28, 0);
    lv_obj_set_style_pad_row(p, 16, 0);
    lv_obj_set_width(p, jm->portrait ? jm->W - 120 : 520);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, jm->H - 60, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(p);
    if (title) {
        lv_obj_t *t = jm_label(p, title, aos_font_title, C(0xF4E8D4));
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (sub) {
        lv_obj_t *t = jm_label(p, sub, aos_font_body, C(0xB8A890));
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    jm->panel = dim;
    jm->panel_kind = kind;
    s_pbox = p;
    return p;
}

static lv_obj_t *panel_btn(lv_obj_t *p, const char *text, lv_event_cb_t cb, void *ud, bool primary)
{
    lv_obj_t *b = jm_button(p, text, cb, ud);
    lv_obj_set_width(b, lv_pct(100));
    if (primary) lv_obj_set_style_bg_color(b, AOS_C_ACCENT, 0);
    return b;
}

static void panel_pad(void)
{
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(s_pbox);
    for (uint32_t i = 0; i < cnt && n < AOS_PAD_MENU_MAX; i++) {
        lv_obj_t *c = lv_obj_get_child(s_pbox, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) items[n++] = c;
    }
    aos_pad_menu_set(&jm->menu, items, n, 0);
    aos_pad_reset(&jm->pad, lv_tick_get());
}

static void ev_close(lv_event_t *e)
{
    (void)e;
    jm_hud_panel_close();
}

static void ev_new(lv_event_t *e)
{
    (void)e;
    int gi = P->gi;
    jm_hud_panel_close();
    jm_setup_open(gi);
}

static void ev_again(lv_event_t *e)
{
    (void)e;
    jm_new_t n;
    memset(&n, 0, sizeof n);
    n.gi = P->gi;
    memcpy(n.opts, P->opts, sizeof n.opts);
    /* the people change sides against the board */
    int np = P->np;
    for (int i = 0; i < np; i++) {
        n.human[i] = P->human[np == 2 ? 1 - i : i];
        n.level[i] = P->level[np == 2 ? 1 - i : i];
    }
    n.two = P->two;
    n.clock_mode = P->clock.mode;
    n.clock_base = P->clock.base_ms;
    n.clock_inc = P->clock.inc_ms;
    jm_hud_panel_close();
    jm_play_start(&n);
}

static void ev_leave(lv_event_t *e)
{
    (void)e;
    jm_hud_panel_close();
    jm_play_stop(true);
    jm_home_show();
}

static void ev_resign(lv_event_t *e)
{
    (void)e;
    jm_hud_panel_close();
    jm_play_resign();
}

static void ev_settings(lv_event_t *e)
{
    (void)e;
    jm_hud_panel_close();
    jm_settings_open();
}

static void ev_save_pgn(lv_event_t *e)
{
    (void)e;
    char name[64];
    jm_hud_panel_close();
    if (jm_pgn_write(name, sizeof name)) {
        char b[120];
        snprintf(b, sizeof b, "%s %s", _("Guardada en"), name);
        aos_ui_toast(b, 2500);
    } else {
        aos_ui_toast(_("No se pudo guardar"), 2000);
    }
}

static char s_files[24][40];

static void ev_open_file(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    jm_hud_panel_close();
    if (!jm_pgn_read(s_files[i])) aos_ui_toast(_("No se pudo leer la partida"), 2000);
}

static void ev_open_pgn(lv_event_t *e)
{
    (void)e;
    int n = jm_pgn_list(s_files, 24);
    lv_obj_t *p = panel_box(PANEL_FILES, _("Abrir una partida"), n ? NULL : _("No hay partidas guardadas en /sdcard/mesa/ajedrez"));
    for (int i = 0; i < n; i++) panel_btn(p, s_files[i], ev_open_file, (void *)(intptr_t)i, false);
    panel_btn(p, _("Cancelar"), ev_close, NULL, false);
    panel_pad();
}

void jm_hud_pause(void)
{
    if (!jm->playing) return;
    lv_obj_t *p = panel_box(PANEL_PAUSE, _(P->g->name), NULL);
    panel_btn(p, _("Seguir"), ev_close, NULL, true);
    panel_btn(p, _("Partida nueva"), ev_new, NULL, false);
    if (!P->over) panel_btn(p, _("Abandonar"), ev_resign, NULL, false);
    if (P->g->flags & MESA_GF_PGN) {
        panel_btn(p, _("Guardar la partida"), ev_save_pgn, NULL, false);
        panel_btn(p, _("Abrir una partida"), ev_open_pgn, NULL, false);
    }
    panel_btn(p, _("Ajustes"), ev_settings, NULL, false);
    panel_btn(p, _("Salir al menú"), ev_leave, NULL, false);
    if (face_to_face() && P->bottom == 0 && jm_play_turn() == 1) turn_round(p);
    panel_pad();
}

void jm_hud_end(void)
{
    if (!jm->playing) return;
    const char *why = NULL;
    int r = P->g->result(P->st, &why);
    char title[96], sub[96];
    sub[0] = 0;
    if (P->clock.flagged >= 0) {
        r = 1 - P->clock.flagged;
        why = N_("Se acabó el tiempo");
    } else if (P->resigned >= 0) {
        r = P->np == 2 ? 1 - P->resigned : -1;
        why = N_("Abandono");
    }
    int people = 0, person = -1;
    for (int i = 0; i < P->np; i++) {
        if (P->human[i]) {
            people++;
            person = i;
        }
    }
    if (r == -2) snprintf(title, sizeof title, "%s", _("Tablas"));
    else if (r >= 0 && people == 1) snprintf(title, sizeof title, "%s", r == person ? _("¡Ganaste!") : _("Ganó la placa"));
    else if (r >= 0) snprintf(title, sizeof title, "%s: %s", _("Gana"), _(P->g->side(P->opts, r, NULL)));
    else snprintf(title, sizeof title, "%s", _("Partida terminada"));
    if (why) snprintf(sub, sizeof sub, "%s", _(why));
    lv_obj_t *p = panel_box(PANEL_END, title, sub[0] ? sub : NULL);
    panel_btn(p, _("Revancha"), ev_again, NULL, true);
    panel_btn(p, _("Ver el tablero"), ev_close, NULL, false);
    if (P->g->flags & MESA_GF_PGN) panel_btn(p, _("Guardar la partida"), ev_save_pgn, NULL, false);
    panel_btn(p, _("Salir al menú"), ev_leave, NULL, false);
    panel_pad();
}

static mesa_move_t s_choice[8];

static void ev_choice(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    jm_play_choose(&s_choice[i]);
}

static void ev_choice_cancel(lv_event_t *e)
{
    (void)e;
    jm_hud_panel_close();
    P->sel = 0;
}

void jm_hud_choice(const mesa_move_t *moves, int n)
{
    lv_obj_t *p = panel_box(PANEL_CHOICE, _("¿En qué se convierte?"), NULL);
    for (int i = 0; i < n && i < 8; i++) {
        s_choice[i] = moves[i];
        int k = P->g->choice_kind ? P->g->choice_kind(P->st, &moves[i]) : 0;
        const char *nm = k >= 0 && k < P->g->nkinds && P->g->kinds[k].name ? _(P->g->kinds[k].name) : "?";
        panel_btn(p, nm, ev_choice, (void *)(intptr_t)i, i == 0);
    }
    panel_btn(p, _("Cancelar"), ev_choice_cancel, NULL, false);
    if (face_to_face() && jm_play_turn() == 1 - P->bottom) turn_round(p);
    panel_pad();
}

bool jm_hud_back(void)
{
    if (jm->panel) {
        if (jm->panel_kind == PANEL_CHOICE) P->sel = 0;
        jm_hud_panel_close();
        return true;
    }
    if (jm->sheet && jm->portrait && !lv_obj_has_flag(jm->sheet, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(jm->sheet, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    jm_hud_pause();
    return true;
}

void jm_hud_pad(void)
{
    aos_pad_t *pad = &jm->pad;
    if (jm->panel) {
        if (aos_pad_pressed(pad, AOS_PAD_B) || (aos_pad_pressed(pad, AOS_PAD_START) && jm->panel_kind == PANEL_PAUSE)) {
            jm_hud_back();
            return;
        }
        aos_pad_menu_step(&jm->menu, pad);
        return;
    }
    if (aos_pad_pressed(pad, AOS_PAD_START)) {
        jm_hud_pause();
        return;
    }
    jm_play_pad();
}
