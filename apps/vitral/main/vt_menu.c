/*
 * VITRAL - the panels: the title, the puzzle's levels, the options, how to
 * play, the pause and the ends. LVGL over the canvas; the gamepad goes
 * through their buttons (aos_pad_menu.h), B goes back.
 */
#include "vt_app.h"
#include "vt_snd.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define GOLD    0xD9B45A
#define INK     0xF4EEDC
#define CARD    0x15131C

static lv_obj_t *s_items[AOS_PAD_MENU_MAX];
static int       s_nitems;

static void item(lv_obj_t *b)
{
    if (s_nitems < AOS_PAD_MENU_MAX) s_items[s_nitems++] = b;
}

static void style_btn(lv_obj_t *b, bool primary)
{
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? 0x3A2E14 : 0x24212C), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? 0x5A4720 : 0x38343F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, 235, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(primary ? GOLD : 0x6B6274), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16141B), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, lv_color_hex(0x34303A), LV_STATE_DISABLED);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t user, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    style_btn(b, primary);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, 76);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(primary ? 0xFFE6A6 : INK), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)user);
    item(b);
    return b;
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void tap_sound(void)
{
    vt_snd_play(VT_SND_TAP, 200);
}

void vt_menu_close(vt_app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    if (a->panel) lv_obj_delete(a->panel);
    a->panel = NULL;
    a->panel_kind = PANEL_NONE;
}

/* a card in the middle of the screen, over a veil */
static lv_obj_t *card(vt_app_t *a, vt_panel_t kind, int width)
{
    vt_menu_close(a);
    s_nitems = 0;
    a->panel_kind = kind;
    a->panel = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->panel);
    lv_obj_set_size(a->panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(a->panel, lv_color_hex(0x05040A), 0);
    lv_obj_set_style_bg_opa(a->panel, 150, 0);
    lv_obj_add_flag(a->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *c = lv_obj_create(a->panel);
    lv_obj_set_width(c, width);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(c, lv_pct(94), 0);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(CARD), 0);
    lv_obj_set_style_bg_opa(c, 245, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(GOLD), 0);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

static void done(vt_app_t *a, int sel)
{
    aos_pad_menu_set(&a->menu, s_items, s_nitems, sel);
    aos_pad_reset(&a->mpad, lv_tick_get());
}

/* -------------------------------------------------------------------------- */
/* The title                                                                   */

static void on_mode(lv_event_t *e)
{
    vt_app_t *a = vt_A;
    intptr_t m = (intptr_t)lv_event_get_user_data(e);
    tap_sound();
    if (m == VT_MODE_PUZZLE) {
        a->panel_from = PANEL_TITLE;
        extern void vt_menu_levels(vt_app_t *);
        vt_menu_levels(a);
        return;
    }
    vt_start(a, (vt_mode_t)m, a->start_level);
}

static void on_options(lv_event_t *e)
{
    vt_app_t *a = vt_A;
    tap_sound();
    a->panel_from = (vt_panel_t)(intptr_t)lv_event_get_user_data(e);
    extern void vt_menu_options(vt_app_t *);
    vt_menu_options(a);
}

static void on_help(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    tap_sound();
    extern void vt_menu_help(vt_app_t *);
    vt_menu_help(a);
}

void vt_menu_title(vt_app_t *a)
{
    vt_menu_close(a);
    s_nitems = 0;
    a->panel_kind = PANEL_TITLE;
    a->panel = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->panel);
    lv_obj_set_size(a->panel, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(a->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = lv_obj_create(a->panel);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    if (!a->land) {
        lv_obj_set_size(col, 560, 1280 - 680);
        lv_obj_set_pos(col, (lv_obj_get_width(a->root) - 560) / 2, 664);
    } else {
        lv_obj_set_size(col, 500, 700);
        lv_obj_set_pos(col, 740, 10);
    }

    lv_obj_t *t = text(col, "VITRAL", aos_font_huge, 0xF2D58A);
    lv_obj_set_style_text_letter_space(t, 10, 0);
    char rec[96];
    if (a->hi_marathon) {
        char n[24];
        uint32_t v = a->hi_marathon;
        if (v >= 1000) snprintf(n, sizeof n, "%u %03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
        else snprintf(n, sizeof n, "%u", (unsigned)v);
        snprintf(rec, sizeof rec, "%s %s", _("Récord:"), n);
    } else {
        snprintf(rec, sizeof rec, "%s", _("Piezas de vidrio que caen"));
    }
    text(col, rec, aos_font_small, GOLD);

    lv_obj_t *b = button(col, _("Maratón"), on_mode, VT_MODE_MARATHON, true);
    (void)b;
    button(col, _("Contrarreloj"), on_mode, VT_MODE_TIME, false);
    button(col, _("Rompecabezas"), on_mode, VT_MODE_PUZZLE, false);
    button(col, _("De a dos"), on_mode, VT_MODE_VERSUS, false);
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 76);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *o = button(row, _("Opciones"), on_options, PANEL_TITLE, false);
    lv_obj_set_flex_grow(o, 1);
    lv_obj_set_width(o, 10);
    lv_obj_t *h = button(row, _("Cómo se juega"), on_help, 0, false);
    lv_obj_set_flex_grow(h, 1);
    lv_obj_set_width(h, 10);
    done(a, 0);
}

/* -------------------------------------------------------------------------- */
/* The puzzle's levels                                                         */

static void on_level(lv_event_t *e)
{
    vt_app_t *a = vt_A;
    tap_sound();
    vt_start(a, VT_MODE_PUZZLE, (int)(intptr_t)lv_event_get_user_data(e));
}

static void on_back(lv_event_t *e)
{
    (void)e;
    tap_sound();
    vt_menu_back(vt_A);
}

void vt_menu_levels(vt_app_t *a)
{
    lv_obj_t *c = card(a, PANEL_LEVELS, a->land ? 1000 : 660);
    text(c, _("Rompecabezas"), aos_font_title, 0xF2D58A);
    text(c, _("Llená el vitral con las piezas que te tocan, sin que sobre nada."), aos_font_small, INK);
    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    int side = a->land ? 84 : 92;
    int sel = 0;
    for (int i = 0; i < VT_PZ_LEVELS; i++) {
        bool open = i < a->pz_open;
        lv_obj_t *b = lv_button_create(grid);
        style_btn(b, open && a->pz_stars[i] == 0);
        lv_obj_set_size(b, side, side);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_t *n = lv_label_create(b);
        char t[8];
        snprintf(t, sizeof t, "%d", i + 1);
        lv_label_set_text(n, open ? t : AOS_SYM_LOCK);
        lv_obj_set_style_text_font(n, open ? aos_font_body : &aos_sym_28, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(open ? INK : 0x6B6274), 0);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, open ? -14 : 0);
        if (open) {
            lv_obj_t *st = lv_label_create(b);
            char s3[40] = "";
            for (int k = 0; k < 3; k++) strcat(s3, k < a->pz_stars[i] ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE);
            lv_label_set_text(st, s3);
            lv_obj_set_style_text_font(st, &aos_sym_28, 0);
            lv_obj_set_style_transform_scale(st, 220, 0);
            lv_obj_set_style_transform_pivot_x(st, lv_pct(50), 0);
            lv_obj_set_style_text_color(st, lv_color_hex(a->pz_stars[i] ? 0xFFD060 : 0x6B6274), 0);
            lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, 2);
            lv_obj_add_event_cb(b, on_level, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            if (i == a->pz_level) sel = s_nitems;
            item(b);
        } else {
            /* not LV_STATE_DISABLED: the theme greys a disabled button
             * light; a locked level is dark stone, and the pad skips it */
            lv_obj_set_style_bg_color(b, lv_color_hex(0x16141B), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0x34303A), 0);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    button(c, _("Volver"), on_back, 0, false);
    done(a, sel);
}

/* -------------------------------------------------------------------------- */
/* Options                                                                     */

static void options_refresh(vt_app_t *a);

static lv_obj_t *s_opt[4];

static void on_opt(lv_event_t *e)
{
    vt_app_t *a = vt_A;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    tap_sound();
    if (k == 0) a->use_keys = !a->use_keys;
    if (k == 1) {
        a->sound = !a->sound;
        vt_snd_enable(a->sound);
    }
    if (k == 2) a->ghost = !a->ghost;
    if (k == 3) a->start_level = a->start_level >= 15 ? 1 : a->start_level == 1 ? 5 : a->start_level + 5;
    vt_prefs_save(a);
    options_refresh(a);
    if (k == 0 && a->screen == SCR_GAME) {
        /* the keys change the layout: the game is drawn again under the panel */
        extern void vt_relayout(vt_app_t *);
        vt_relayout(a);
    }
}

static void options_refresh(vt_app_t *a)
{
    char b[96];
    snprintf(b, sizeof b, "%s: %s", _("Controles"), a->use_keys ? _("teclas en pantalla") : _("gestos"));
    lv_label_set_text(lv_obj_get_child(s_opt[0], 0), b);
    snprintf(b, sizeof b, "%s: %s", _("Sonido"), a->sound ? _("sí") : _("no"));
    lv_label_set_text(lv_obj_get_child(s_opt[1], 0), b);
    snprintf(b, sizeof b, "%s: %s", _("Sombra de la pieza"), a->ghost ? _("sí") : _("no"));
    lv_label_set_text(lv_obj_get_child(s_opt[2], 0), b);
    snprintf(b, sizeof b, "%s: %d", _("Nivel inicial"), a->start_level);
    lv_label_set_text(lv_obj_get_child(s_opt[3], 0), b);
}

void vt_menu_options(vt_app_t *a)
{
    lv_obj_t *c = card(a, PANEL_OPTIONS, a->land ? 760 : 640);
    text(c, _("Opciones"), aos_font_title, 0xF2D58A);
    for (int i = 0; i < 4; i++) s_opt[i] = button(c, "", on_opt, i, false);
    options_refresh(a);
    button(c, _("Listo"), on_back, 0, true);
    done(a, 0);
}

/* -------------------------------------------------------------------------- */
/* How to play                                                                 */

void vt_menu_help(vt_app_t *a)
{
    lv_obj_t *c = card(a, PANEL_HELP, a->land ? 1100 : 660);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    text(c, _("Cómo se juega"), aos_font_title, 0xF2D58A);
    lv_obj_t *t = text(c, _("Caen piezas de vidrio de tres y de cinco cuadros. Una fila completa se rompe y suma puntos; "
                            "varias juntas suman más. Una fila completa de un solo color es un vitral: vale el triple "
                            "y se lleva también la fila de abajo. El vidrio transparente va con cualquier color."),
                       aos_font_small, INK);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    t = text(c, _("Con el dedo: deslizá a los costados para mover la pieza y hacia abajo para bajarla; "
                  "un golpe rápido hacia abajo la tira, uno hacia arriba la guarda. Un toque la gira: "
                  "en la mitad derecha a la derecha, en la izquierda al revés."),
             aos_font_small, INK);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    t = text(c, _("Con joystick: la cruz mueve y baja, arriba la tira, A y B giran, L o R la guardan y START pausa. "
                  "Con teclado: flechas, Z y X giran, espacio la tira y C la guarda."),
             aos_font_small, INK);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    button(c, _("Volver"), on_back, 0, true);
    done(a, 0);
}

/* -------------------------------------------------------------------------- */
/* Pause and the ends                                                          */

static void on_resume(lv_event_t *e)
{
    (void)e;
    tap_sound();
    vt_set_paused(vt_A, false);
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    tap_sound();
    vt_start(a, a->mode, a->mode == VT_MODE_PUZZLE ? a->pz_level : a->start_level);
}

static void on_next(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    tap_sound();
    int n = a->pz_level + 1;
    if (n >= VT_PZ_LEVELS) n = 0;
    vt_start(a, VT_MODE_PUZZLE, n);
}

static void on_levels(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    tap_sound();
    vt_quit_to_title(a);
    a->panel_from = PANEL_TITLE;
    vt_menu_levels(a);
}

static void on_menu(lv_event_t *e)
{
    (void)e;
    tap_sound();
    vt_quit_to_title(vt_A);
}

static void on_undo_panel(lv_event_t *e)
{
    (void)e;
    vt_app_t *a = vt_A;
    tap_sound();
    vt_menu_close(a);
    vt_pz_undo(&a->p[0].g);
    a->over = false;
    a->end_t = -1;
}

void vt_menu_pause(vt_app_t *a)
{
    lv_obj_t *c = card(a, PANEL_PAUSE, a->land ? 620 : 560);
    text(c, _("Pausa"), aos_font_title, 0xF2D58A);
    button(c, _("Seguir"), on_resume, 0, true);
    button(c, _("Empezar de nuevo"), on_restart, 0, false);
    if (a->mode == VT_MODE_PUZZLE) button(c, _("Niveles"), on_levels, 0, false);
    button(c, _("Opciones"), on_options, PANEL_PAUSE, false);
    button(c, _("Menú"), on_menu, 0, false);
    done(a, 0);
}

static void fmt_pts(char *b, size_t n, uint32_t v)
{
    if (v >= 1000000) snprintf(b, n, "%u %03u %03u", (unsigned)(v / 1000000), (unsigned)(v / 1000 % 1000), (unsigned)(v % 1000));
    else if (v >= 1000) snprintf(b, n, "%u %03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
    else snprintf(b, n, "%u", (unsigned)v);
}

void vt_menu_over(vt_app_t *a)
{
    char b[96], n[24];
    vt_game_t *g = &a->p[0].g;
    if (a->mode == VT_MODE_VERSUS) {
        lv_obj_t *c = card(a, PANEL_VS_OVER, a->land ? 640 : 560);
        snprintf(b, sizeof b, _("Ganó el jugador %d"), a->winner + 1);
        text(c, b, aos_font_title, 0xF2D58A);
        snprintf(b, sizeof b, "%d  -  %d", a->wins[0], a->wins[1]);
        text(c, b, aos_font_large, INK);
        button(c, _("Revancha"), on_restart, 0, true);
        button(c, _("Menú"), on_menu, 0, false);
        done(a, 0);
        return;
    }
    if (a->mode == VT_MODE_PUZZLE) {
        if (g->state == VT_WON) {
            lv_obj_t *c = card(a, PANEL_WON, a->land ? 640 : 560);
            text(c, _("¡Vitral completo!"), aos_font_title, 0xF2D58A);
            char s3[40] = "";
            for (int k = 0; k < 3; k++) strcat(s3, k < a->stars_now ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE);
            lv_obj_t *st = text(c, s3, &aos_sym_44, 0xFFD060);
            (void)st;
            int secs = (int)(g->time_ms / 1000);
            snprintf(b, sizeof b, "%s %d:%02d", _("Tiempo:"), secs / 60, secs % 60);
            text(c, b, aos_font_small, INK);
            button(c, _("Siguiente"), on_next, 0, true);
            button(c, _("Otra vez"), on_restart, 0, false);
            button(c, _("Niveles"), on_levels, 0, false);
            done(a, 0);
        } else {
            lv_obj_t *c = card(a, PANEL_STUCK, a->land ? 640 : 560);
            text(c, _("Se acabaron las piezas"), aos_font_title, 0xF2D58A);
            text(c, _("Quedó vidrio sin llenar. Deshacé la última o empezá de nuevo."), aos_font_small, INK);
            button(c, _("Deshacer"), on_undo_panel, 0, true);
            button(c, _("Empezar de nuevo"), on_restart, 0, false);
            button(c, _("Niveles"), on_levels, 0, false);
            done(a, 0);
        }
        return;
    }
    lv_obj_t *c = card(a, PANEL_OVER, a->land ? 640 : 560);
    bool timeup = a->mode == VT_MODE_TIME && g->time_ms >= g->time_limit;
    text(c, timeup ? _("¡Tiempo!") : _("Se llenó el vitral"), aos_font_title, 0xF2D58A);
    fmt_pts(n, sizeof n, g->score);
    text(c, n, aos_font_large, INK);
    if (a->new_record) text(c, _("¡Récord nuevo!"), aos_font_body, 0xFFD060);
    else if (a->mode == VT_MODE_TIME ? a->hi_time : a->hi_marathon) {
        fmt_pts(n, sizeof n, a->mode == VT_MODE_TIME ? a->hi_time : a->hi_marathon);
        snprintf(b, sizeof b, "%s %s", _("Récord:"), n);
        text(c, b, aos_font_small, GOLD);
    }
    snprintf(b, sizeof b, _("%d filas, %d vitrales, nivel %d"), g->lines, g->vitrals, g->level);
    text(c, b, aos_font_small, INK);
    button(c, _("Otra vez"), on_restart, 0, true);
    button(c, _("Menú"), on_menu, 0, false);
    done(a, 0);
}

/* -------------------------------------------------------------------------- */

bool vt_menu_back(vt_app_t *a)
{
    switch (a->panel_kind) {
    case PANEL_LEVELS:
    case PANEL_HELP:
        vt_menu_title(a);
        return true;
    case PANEL_OPTIONS:
        if (a->panel_from == PANEL_PAUSE) vt_menu_pause(a);
        else vt_menu_title(a);
        return true;
    case PANEL_PAUSE:
        vt_set_paused(a, false);
        return true;
    default:
        return false;
    }
}

void vt_menu_frame(vt_app_t *a)
{
    if (!a->panel) return;
    aos_pad_update(&a->mpad, lv_tick_get());
    if (aos_pad_menu_step(&a->menu, &a->mpad)) return;
    if (aos_pad_pressed(&a->mpad, AOS_PAD_B)) {
        vt_menu_back(a);
        return;
    }
    if (aos_pad_pressed(&a->mpad, AOS_PAD_START)) {
        if (a->panel_kind == PANEL_PAUSE) vt_set_paused(a, false);
        else if (a->panel_kind == PANEL_TITLE) vt_start(a, VT_MODE_MARATHON, a->start_level);
    }
}
