/*
 * CIELO - the panels over the sky: the settings, the place, the date, and
 * the satellites (their passes, their elements, which ones to follow).
 *
 * Ordinary LVGL screens that cover the sky while they are up (the frames
 * stop: nothing is flipped over them), built the way Vuelos builds its
 * own (vl_ui.c). They are rebuilt when the screen turns.
 */
#include "cielo.h"
#include "cl_dmath.h"

#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD     24
#define ROW_H   96
#define ROW_H1  80

/* ---------------------------------------------------------------------------
 * Common
 * ------------------------------------------------------------------------- */

void cl_show_sky(app_t *a)
{
    if (a->panel) {
        lv_obj_delete(a->panel);
        a->panel = NULL;
    }
    a->list_box = NULL;
    memset(a->date_rollers, 0, sizeof a->date_rollers);
    memset(a->place_ta, 0, sizeof a->place_ta);
    a->screen = SCR_SKY;
    a->viewing = true;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    lv_obj_remove_flag(a->sky, LV_OBJ_FLAG_HIDDEN);
    cl_dirty(a);
}

static void back_cb(lv_event_t *e)
{
    cl_show_sky((app_t *)lv_event_get_user_data(e));
}

static lv_obj_t *panel_new(app_t *a, int screen)
{
    if (a->panel) lv_obj_delete(a->panel);
    a->list_box = NULL;
    memset(a->date_rollers, 0, sizeof a->date_rollers);
    memset(a->place_ta, 0, sizeof a->place_ta);
    a->viewing = false;
    a->screen = screen;
    a->touching = a->pinching = a->anim = false;
    a->vaz = a->valt = 0;
    a->self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    lv_obj_add_flag(a->sky, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *p = lv_obj_create(a->root);
    a->panel = p;
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, a->sw, a->sh);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 80, 80);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = aos_label(b, sym, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user, bool accent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_pad_hor(b, 30, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, accent ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *t = aos_label(b, text, aos_font_body, AOS_C_TEXT);
    lv_obj_center(t);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *row(lv_obj_t *parent, int w, const char *sym, uint32_t dot, const char *text, const char *sub,
                     const char *val, lv_event_cb_t cb, void *user, int idx)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, sub ? ROW_H : ROW_H1);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 20, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    }
    lv_obj_set_user_data(r, (void *)(intptr_t)idx);
    int tx = 24;
    if (sym) {
        lv_obj_t *ic = aos_label(r, sym, &aos_sym_28, AOS_C_ACCENT);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        tx = 70;
    } else if (dot) {
        lv_obj_t *d = lv_obj_create(r);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 18, 18);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(dot), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 26, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        tx = 66;
    }
    int vw = 0;
    if (val) {
        lv_obj_t *v = aos_label(r, val, aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        vw = w * 6 / 25;
        lv_obj_set_width(v, vw);
        lv_obj_set_height(v, lv_font_get_line_height(aos_font_body));
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
        vw += 12;
    }
    lv_obj_t *l = aos_label(r, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - tx - 20 - vw);
    lv_obj_set_height(l, lv_font_get_line_height(aos_font_body));
    lv_obj_align(l, sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, tx, sub ? 14 : 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (sub) {
        lv_obj_t *s = aos_label(r, sub, aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, w - tx - 20 - vw);
        lv_obj_set_height(s, lv_font_get_line_height(aos_font_small));
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, tx, -12);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    }
    return r;
}

static void heading(lv_obj_t *parent, int w, const char *text)
{
    lv_obj_t *h = aos_label(parent, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(h, w - 16);
    lv_obj_set_style_pad_top(h, 18, 0);
}

static void note(lv_obj_t *parent, int w, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(t, 18, 0);
}

static lv_obj_t *column(app_t *a, lv_obj_t *p, int top)
{
    lv_obj_t *l = lv_obj_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, a->sw, a->sh - top);
    lv_obj_set_pos(l, 0, top);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 12, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_obj_set_style_pad_bottom(l, 80, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    return l;
}

static int col_w(const app_t *a)
{
    int w = a->sw - 2 * PAD;
    return w > 760 ? 760 : w;
}

static lv_obj_t *titled(app_t *a, int screen, const char *title)
{
    lv_obj_t *p = panel_new(a, screen);
    lv_obj_t *back = round_btn(p, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, PAD, PAD);
    lv_obj_t *t = aos_label(p, title, aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, PAD + 104, PAD + 18);
    return p;
}

/* ---------------------------------------------------------------------------
 * The settings
 * ------------------------------------------------------------------------- */

enum {
    SET_NIGHT = 0, SET_LINES, SET_CNAMES, SET_SNAMES, SET_MW, SET_MESSIER, SET_PLANETS, SET_SATS, SET_GRID,
    SET_GROUND, SET_ATMOS, SET_MAG, SET_ALERTS, SET_ALERT_MIN, SET_PLACE, SET_SATLIST,
};

static bool *flag_of(app_t *a, int what)
{
    cl_conf_t *c = &a->conf;
    switch (what) {
    case SET_NIGHT: return &c->night;
    case SET_LINES: return &c->lines;
    case SET_CNAMES: return &c->cnames;
    case SET_SNAMES: return &c->snames;
    case SET_MW: return &c->mw;
    case SET_MESSIER: return &c->messier;
    case SET_PLANETS: return &c->planets;
    case SET_SATS: return &c->sats;
    case SET_GRID: return &c->grid;
    case SET_GROUND: return &c->ground;
    case SET_ATMOS: return &c->atmos;
    case SET_ALERTS: return &c->alerts;
    default: return NULL;
    }
}

static void settings_build(app_t *a);

static void set_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int what = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    bool *f = flag_of(a, what);
    if (f) {
        *f = !*f;
        if (what == SET_ALERTS) {
            if (a->conf.alerts) a->self->desc.flags |= AOS_APP_FLAG_BACKGROUND;
            else a->self->desc.flags &= ~(uint32_t)AOS_APP_FLAG_BACKGROUND;
        }
    } else if (what == SET_MAG) {
        static const float STEPS[] = { -1.5f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
        int i = 0;
        while (i < 6 && STEPS[i] < a->conf.mag_extra + 0.01f) i++;
        a->conf.mag_extra = STEPS[i % 6];
    } else if (what == SET_ALERT_MIN) {
        static const int STEPS[] = { 2, 5, 10, 15, 30 };
        int i = 0;
        while (i < 5 && STEPS[i] <= a->conf.alert_min) i++;
        a->conf.alert_min = STEPS[i % 5];
    } else if (what == SET_PLACE) {
        cl_show_place(a);
        return;
    } else if (what == SET_SATLIST) {
        cl_show_sats(a);
        return;
    }
    cl_conf_save(a);
    settings_build(a);
}

static void settings_build(app_t *a)
{
    lv_obj_t *l = a->list_box;
    if (!l) return;
    int32_t sy = lv_obj_get_scroll_y(l);
    lv_obj_clean(l);
    int w = col_w(a);
    static const struct {
        int what;
        const char *name, *sub;
    } ROWS[] = {
        { SET_NIGHT, N_("Modo nocturno"), N_("Todo en rojo, para no perder la vista") },
        { SET_LINES, N_("Figuras de las constelaciones"), NULL },
        { SET_CNAMES, N_("Nombres de las constelaciones"), NULL },
        { SET_SNAMES, N_("Nombres de las estrellas"), NULL },
        { SET_MW, N_("Vía Láctea"), NULL },
        { SET_MESSIER, N_("Objetos de Messier"), N_("Cúmulos, nebulosas y galaxias") },
        { SET_PLANETS, N_("Planetas"), NULL },
        { SET_SATS, N_("Satélites"), NULL },
        { SET_GRID, N_("Cuadrícula de altura y acimut"), NULL },
        { SET_GROUND, N_("Suelo"), N_("Tapa lo que está bajo el horizonte") },
        { SET_ATMOS, N_("Cielo de día"), N_("El cielo se aclara con el Sol") },
        { SET_MAG, N_("Cuántas estrellas"), N_("Más o menos débiles que lo normal") },
    };
    char v[32];
    heading(l, w, _("Qué se ve"));
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        bool *f = flag_of(a, ROWS[i].what);
        if (f) snprintf(v, sizeof v, "%s", *f ? _("Sí") : _("No"));
        else snprintf(v, sizeof v, "%+.1f mag", (double)a->conf.mag_extra);
        row(l, w, NULL, 0, _(ROWS[i].name), ROWS[i].sub ? _(ROWS[i].sub) : NULL, v, set_cb, a, ROWS[i].what);
    }
    heading(l, w, _("Pasadas"));
    row(l, w, AOS_SYM_BELL_OUTLINE, 0, _("Avisos de pasadas visibles"), _("También con la app en segundo plano"),
        a->conf.alerts ? _("Sí") : _("No"), set_cb, a, SET_ALERTS);
    snprintf(v, sizeof v, "%d min", a->conf.alert_min);
    row(l, w, AOS_SYM_TIMER_OUTLINE, 0, _("Cuánto antes"), NULL, v, set_cb, a, SET_ALERT_MIN);
    row(l, w, AOS_SYM_UPDATE, 0, _("Satélites y órbitas"), NULL, NULL, set_cb, a, SET_SATLIST);
    heading(l, w, _("Lugar"));
    char sub[80];
    snprintf(sub, sizeof sub, "%.4f, %.4f", (double)a->conf.lat, (double)a->conf.lon);
    row(l, w, AOS_SYM_EARTH, 0, a->conf.place, sub, NULL, set_cb, a, SET_PLACE);

    note(l, w, _("Estrellas: Yale Bright Star Catalogue. Objetos de Messier: HEASARC (NASA). Figuras y Vía Láctea: "
                 "propias de Cielo. Órbitas: CelesTrak. En la página Cielo del portal se busca el lugar por su "
                 "nombre y se ven las pasadas."));
    lv_obj_update_layout(l);
    lv_obj_scroll_to_y(l, sy, LV_ANIM_OFF);
}

void cl_show_settings(app_t *a)
{
    lv_obj_t *p = titled(a, SCR_SETTINGS, _("Ajustes"));
    a->list_box = column(a, p, PAD + 100);
    settings_build(a);
}

/* ---------------------------------------------------------------------------
 * The place
 * ------------------------------------------------------------------------- */

static lv_obj_t *s_kb;

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *field(app_t *a, lv_obj_t *parent, int w, const char *label, const char *text, bool numeric)
{
    (void)a;
    lv_obj_t *t = aos_label(parent, label, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, w);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, w);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, text);
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 16, 0);
    lv_obj_set_style_pad_all(ta, 16, 0);
    if (numeric) lv_textarea_set_accepted_chars(ta, "0123456789.-");
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, a);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, a);
    return ta;
}

static void place_save_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->place_ta[0]) return;
    const char *nm = lv_textarea_get_text(a->place_ta[0]);
    float lat = strtof(lv_textarea_get_text(a->place_ta[1]), NULL);
    float lon = strtof(lv_textarea_get_text(a->place_ta[2]), NULL);
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        aos_ui_toast(_("Latitud entre -90 y 90, longitud entre -180 y 180"), 2500);
        return;
    }
    snprintf(a->conf.place, sizeof a->conf.place, "%s", nm[0] ? nm : _("Mi lugar"));
    a->conf.lat = lat;
    a->conf.lon = lon;
    a->conf.have_place = true;
    cl_conf_save(a);
    cl_sat_replan(a);
    cl_show_sky(a);
}

static void place_clima_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int32_t la, lo;
    if (!aos_hal_pref_get_i32("clima_lat", &la) || !aos_hal_pref_get_i32("clima_lon", &lo)) {
        aos_ui_toast(_("Clima no tiene un lugar elegido"), 2000);
        return;
    }
    char city[48] = "", t[24];
    aos_hal_pref_get_str("clima_city", city, sizeof city);
    lv_textarea_set_text(a->place_ta[0], city);
    snprintf(t, sizeof t, "%.4f", (double)la / 1e4);
    lv_textarea_set_text(a->place_ta[1], t);
    snprintf(t, sizeof t, "%.4f", (double)lo / 1e4);
    lv_textarea_set_text(a->place_ta[2], t);
}

void cl_show_place(app_t *a)
{
    lv_obj_t *p = titled(a, SCR_PLACE, _("Lugar"));
    lv_obj_t *l = column(a, p, PAD + 100);
    a->list_box = l;
    int w = col_w(a);
    char t[24];
    a->place_ta[0] = field(a, l, w, _("Nombre"), a->conf.place, false);
    snprintf(t, sizeof t, "%.4f", (double)a->conf.lat);
    a->place_ta[1] = field(a, l, w, _("Latitud (sur negativa)"), t, true);
    snprintf(t, sizeof t, "%.4f", (double)a->conf.lon);
    a->place_ta[2] = field(a, l, w, _("Longitud (oeste negativa)"), t, true);
    lv_obj_t *btns = lv_obj_create(l);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_set_style_pad_top(btns, 12, 0);
    pill(btns, _("Guardar"), place_save_cb, a, true);
    pill(btns, _("El de Clima"), place_clima_cb, a, false);
    note(l, w, _("En la página Cielo del portal se busca el lugar por su nombre."));
    s_kb = lv_keyboard_create(p);
    lv_obj_set_size(s_kb, a->sw, a->sh * 2 / 5);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_ALL, a);
}

/* ---------------------------------------------------------------------------
 * The date
 * ------------------------------------------------------------------------- */

static void roller_opts(char *buf, size_t n, int from, int to, bool two)
{
    size_t at = 0;
    buf[0] = 0;
    for (int i = from; i <= to && at + 8 < n; i++)
        at += (size_t)snprintf(buf + at, n - at, two ? "%s%02d" : "%s%d", i == from ? "" : "\n", i);
}

static void date_go_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->date_rollers[0]) return;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_mday = (int)lv_roller_get_selected(a->date_rollers[0]) + 1;
    tm.tm_mon = (int)lv_roller_get_selected(a->date_rollers[1]);
    tm.tm_year = (int)lv_roller_get_selected(a->date_rollers[2]) + 1950 - 1900;
    tm.tm_hour = (int)lv_roller_get_selected(a->date_rollers[3]);
    tm.tm_min = (int)lv_roller_get_selected(a->date_rollers[4]) * 5;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    cl_set_time(a, cl_jd_from_unix((double)t));
    a->rate_i = 4;
    a->rate = 1;
    cl_show_sky(a);
}

static void date_now_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    cl_go_live(a);
    cl_show_sky(a);
}

static void date_tonight_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    /* today at 22:00, or tomorrow's if that has passed */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 22;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t < now - 3 * 3600) t += 86400;
    cl_set_time(a, cl_jd_from_unix((double)t));
    a->rate_i = 4;
    a->rate = 1;
    cl_show_sky(a);
}

void cl_show_date(app_t *a)
{
    lv_obj_t *p = titled(a, SCR_DATE, _("Fecha y hora"));
    lv_obj_t *l = column(a, p, PAD + 100);
    a->list_box = l;
    int w = col_w(a);
    time_t t = (time_t)cl_unix_from_jd(cl_now_jd(a));
    struct tm tm;
    localtime_r(&t, &tm);
    lv_obj_t *box = lv_obj_create(l);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, 300);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static char opts[5][700];
    roller_opts(opts[0], sizeof opts[0], 1, 31, false);
    snprintf(opts[1], sizeof opts[1], "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s", _("ene"), _("feb"), _("mar."),
             _("abr"), _("may"), _("jun"), _("jul"), _("ago"), _("sep"), _("oct"), _("nov"), _("dic"));
    roller_opts(opts[2], sizeof opts[2], 1950, 2050, false);
    roller_opts(opts[3], sizeof opts[3], 0, 23, true);
    {
        size_t at = 0;
        for (int m = 0; m < 60; m += 5) at += (size_t)snprintf(opts[4] + at, sizeof opts[4] - at, "%s%02d", m ? "\n" : "", m);
    }
    int sel[5] = { tm.tm_mday - 1, tm.tm_mon, tm.tm_year + 1900 - 1950, tm.tm_hour, tm.tm_min / 5 };
    int widths[5] = { 90, 110, 130, 90, 90 };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *r = lv_roller_create(box);
        lv_roller_set_options(r, opts[i], LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(r, 5);
        lv_obj_set_width(r, widths[i]);
        lv_obj_set_style_text_font(r, aos_font_body, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_text_color(r, AOS_C_TEXT, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_bg_color(r, AOS_C_ACCENT, LV_PART_SELECTED);
        if (sel[i] < 0) sel[i] = 0;
        lv_roller_set_selected(r, (uint32_t)sel[i], LV_ANIM_OFF);
        a->date_rollers[i] = r;
    }
    lv_obj_t *btns = lv_obj_create(l);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_set_style_pad_row(btns, 16, 0);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pill(btns, _("Ir"), date_go_cb, a, true);
    pill(btns, _("Esta noche"), date_tonight_cb, a, false);
    pill(btns, _("Ahora"), date_now_cb, a, false);
    note(l, w, _("Abajo, en el cielo: las flechas mueven una hora (sostenidas, un día) y « » cambian la velocidad del "
                 "tiempo, hacia adelante o hacia atrás."));
}

/* ---------------------------------------------------------------------------
 * The satellites
 * ------------------------------------------------------------------------- */

static void sats_build(app_t *a);

static void pass_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->npasses) return;
    cl_passx_t x = a->passes[i];
    cl_show_sky(a);
    /* to the pass: the sky as it comes into view (lit, if it ever is) */
    double t = x.p.vis_start ? x.p.vis_start + 20.0 / 86400 : x.p.rise;
    cl_set_time(a, t);
    a->rate_i = 4;
    a->rate = 1;
    float fov = a->sw > a->sh ? 200 * 0.017453f : 190 * 0.017453f;
    a->az_target = cl_dome_az(a);
    a->alt_target = 90 * 0.017453f;
    a->fov_target = fov;
    a->anim = true;
    cl_select(a, OBJ_SAT, x.sat);
}

static void fetch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    cl_sat_fetch(a, true);
    aos_ui_toast(_("Bajando las órbitas..."), 1500);
}

static void choose_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->tle_n) return;
    uint32_t id = a->tle_ids[i];
    cl_conf_t *c = &a->conf;
    int at = -1;
    for (int k = 0; k < c->nsat_ids; k++)
        if (c->sat_ids[k] == id) at = k;
    if (at >= 0) {
        memmove(&c->sat_ids[at], &c->sat_ids[at + 1], sizeof(uint32_t) * (size_t)(c->nsat_ids - at - 1));
        c->nsat_ids--;
    } else if (c->nsat_ids < CL_MAX_SATS) {
        c->sat_ids[c->nsat_ids++] = id;
    } else {
        aos_ui_toast(_("Hasta 12 satélites"), 1800);
        return;
    }
    if (a->sel_type == OBJ_SAT) cl_select(a, OBJ_NONE, -1);
    cl_conf_save(a);
    cl_sat_reload(a);
    cl_sat_replan(a);
    sats_build(a);
}

static void sats_build(app_t *a)
{
    lv_obj_t *l = a->list_box;
    if (!l) return;
    int32_t sy = lv_obj_get_scroll_y(l);
    lv_obj_clean(l);
    int w = col_w(a);
    char t1[40], t2[16], sub[96], v[24];

    heading(l, w, _("Próximas pasadas sobre 10°"));
    aos_hal_mutex_lock(a->mutex);
    int shown = 0;
    for (int i = 0; i < a->npasses && shown < 20; i++) {
        const cl_passx_t *x = &a->passes[i];
        if (x->sat >= a->nsats) continue;
        cl_fmt_time(x->p.vis_start ? x->p.vis_start : x->p.rise, t1, sizeof t1, 2);
        cl_fmt_time(x->p.set, t2, sizeof t2, 0);
        snprintf(sub, sizeof sub, "%s-%s · %s %.0f°", t1, t2, _("máx."), (double)(x->p.top_el * 57.29578f));
        if (x->p.vis_start && x->p.mag < 90) snprintf(v, sizeof v, "%s %.1f", _("mag"), (double)x->p.mag);
        else snprintf(v, sizeof v, "%s", x->p.vis_start ? _("visible") : _("no visible"));
        row(l, w, NULL, x->p.vis_start ? 0x5FD0FF : 0x4A5260, a->sats[x->sat].el.name, sub, v, pass_cb, a, i);
        shown++;
    }
    bool busy = a->pass_busy || a->pass_redo;
    aos_hal_mutex_unlock(a->mutex);
    if (!shown) note(l, w, busy ? _("Calculando pasadas...") : a->nsats ? _("Ninguna en los próximos tres días.")
                                                                       : _("Elegí satélites más abajo."));

    heading(l, w, _("Órbitas"));
    if (a->tle_time) {
        long age = (long)time(NULL) - a->tle_time;
        if (age < 3600) snprintf(sub, sizeof sub, "%s %ld min", _("Bajadas hace"), age / 60);
        else if (age < 2 * 86400) snprintf(sub, sizeof sub, "%s %ld h", _("Bajadas hace"), age / 3600);
        else snprintf(sub, sizeof sub, "%s %ld %s", _("Bajadas hace"), age / 86400, _("días"));
    } else {
        snprintf(sub, sizeof sub, "%s", _("Todavía no hay órbitas"));
    }
    if (a->tle_err[0]) snprintf(sub + strlen(sub), sizeof sub - strlen(sub), " · %s", a->tle_err);
    row(l, w, AOS_SYM_DOWNLOAD, 0, _("Actualizar las órbitas"), sub, NULL, fetch_cb, a, 0);

    heading(l, w, _("Satélites a seguir"));
    if (!a->tle_n) note(l, w, _("Cuando estén las órbitas aparece acá la lista para elegir."));
    for (int i = 0; i < a->tle_n; i++) {
        bool on = false;
        for (int k = 0; k < a->conf.nsat_ids; k++)
            if (a->conf.sat_ids[k] == a->tle_ids[i]) on = true;
        snprintf(sub, sizeof sub, "NORAD %u", (unsigned)a->tle_ids[i]);
        row(l, w, on ? AOS_SYM_CHECK : NULL, on ? 0 : 0x333A44, a->tle_names[i], NULL, on ? _("Sí") : "", choose_cb, a, i);
    }
    lv_obj_update_layout(l);
    lv_obj_scroll_to_y(l, sy, LV_ANIM_OFF);
}

void cl_show_sats(app_t *a)
{
    lv_obj_t *p = titled(a, SCR_SATS, _("Satélites"));
    a->list_box = column(a, p, PAD + 100);
    sats_build(a);
}

void cl_ui_relayout(app_t *a)
{
    switch (a->screen) {
    case SCR_SETTINGS: cl_show_settings(a); break;
    case SCR_PLACE: cl_show_place(a); break;
    case SCR_DATE: cl_show_date(a); break;
    case SCR_SATS: cl_show_sats(a); break;
    default: break;
    }
}

/* the satellites' panel follows the passes as they come out of the thread */
void cl_ui_tick(app_t *a, uint32_t now)
{
    static uint32_t last;
    static int last_n = -1;
    static bool last_busy;
    if (a->screen != SCR_SATS || now - last < 1000) return;
    last = now;
    bool busy = a->pass_busy || a->pass_redo;
    if (a->npasses != last_n || busy != last_busy) {
        last_n = a->npasses;
        last_busy = busy;
        sats_build(a);
    }
}
