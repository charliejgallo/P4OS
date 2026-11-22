/*
 * VUELOS - the panels over the map: the list of the aircraft and the
 * settings.
 *
 * Both are ordinary LVGL screens that cover the map while they are up (the
 * frames stop, the map's canvas is hidden under them), as Mapas' menu. They
 * are rebuilt when the screen turns.
 *
 *   list      every aircraft of the last reading, by distance or by
 *             altitude; a row picks it and goes to it on the map. Refreshed
 *             every few seconds, keeping the scroll.
 *   settings  the source, the radius, the trails, the units, the labels,
 *             the routes, the alerts, the place. What needs a keyboard (the
 *             receiver's address, the services' keys, a place by name) is
 *             typed in the portal page.
 */
#include "vuelos.h"

#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD         24
#define ROW_H       96
#define ROW_H1      80

/* ---------------------------------------------------------------------------
 * Common
 * ------------------------------------------------------------------------- */

void vl_show_map(app_t *a)
{
    if (a->panel) {
        lv_obj_delete(a->panel);
        a->panel = NULL;
    }
    a->list_box = NULL;
    a->viewing = true;
    a->screen = SCR_MAP;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    lv_obj_remove_flag(a->map, LV_OBJ_FLAG_HIDDEN);
    a->overlay_dirty = true;
}

static void back_cb(lv_event_t *e)
{
    vl_show_map((app_t *)lv_event_get_user_data(e));
}

static lv_obj_t *panel_new(app_t *a, int screen)
{
    if (a->panel) lv_obj_delete(a->panel);
    a->list_box = NULL;
    a->viewing = false;
    a->screen = screen;
    a->vx = a->vy = 0;
    a->animating = false;
    a->zooming = a->pinching = a->touching = false;
    a->self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    lv_obj_add_flag(a->map, LV_OBJ_FLAG_HIDDEN);
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

/* A card row: an icon (or a coloured dot), a line of text, a second dimmer
 * one, and a value at the right. */
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
        vw = strchr(val, '\n') ? w * 2 / 5 : w * 7 / 25;
        lv_obj_set_width(v, vw);
        lv_obj_set_height(v, lv_font_get_line_height(aos_font_body) * (strchr(val, '\n') ? 2 : 1));
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

/* ---------------------------------------------------------------------------
 * The list
 * ------------------------------------------------------------------------- */

static app_t *s_sort_app;

static int by_dist(const void *x, const void *y)
{
    const vl_ac_t *A = &s_sort_app->ac[*(const int *)x], *B = &s_sort_app->ac[*(const int *)y];
    return A->dist_km < B->dist_km ? -1 : A->dist_km > B->dist_km;
}

static int alt_of(const vl_ac_t *ac)
{
    return ac->ground ? -100000 : ac->has_alt ? (int)ac->alt_ft : -50000;
}

static int by_alt(const void *x, const void *y)
{
    const vl_ac_t *A = &s_sort_app->ac[*(const int *)x], *B = &s_sort_app->ac[*(const int *)y];
    int a = alt_of(A), b = alt_of(B);
    return a > b ? -1 : a < b;
}

static void pick_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->nac || !a->ac[i].used) return;
    vl_select(a, i);
    uint32_t x, y;
    vl_ac_pos(a, &a->ac[i], (uint32_t)aos_hal_uptime_ms(), &x, &y);
    vl_go_to(a, x, y, a->view.z < 9 ? 9 : a->view.z);
    vl_show_map(a);
}

static void list_fill(app_t *a)
{
    lv_obj_t *l = a->list_box;
    if (!l) return;
    int32_t sy = lv_obj_get_scroll_y(l);
    lv_obj_clean(l);
    int w = col_w(a);
    static int idx[VL_MAX_AC];
    int n = 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < a->nac; i++)
        if (a->ac[i].used && !vl_ac_stale(&a->ac[i], now)) idx[n++] = i;
    s_sort_app = a;
    qsort(idx, (size_t)n, sizeof idx[0], a->list_by_alt ? by_alt : by_dist);
    if (!n) {
        /* with no reading yet, what the bar says: looking, or why not. The
         * source goes with a refusal, which is that source's and not the
         * board's: another one may answer. */
        char why[96], line[160];
        uint32_t rgb;
        vl_status_text(a, why, sizeof why, &rgb);
        int e = a->feed_err;
        if (e == 401 || e == 403 || e == 429 || e == AOS_HTTP_ERR_RECV)
            snprintf(line, sizeof line, "%s (%s)", why, vl_src_name(vl_src_now(a)));
        else
            snprintf(line, sizeof line, "%s", why);
        lv_obj_t *t = aos_label(l, a->feed_ok_ms ? _("No hay aviones en el radio elegido.") : line, aos_font_body,
                                AOS_C_DIM);
        lv_obj_set_width(t, w);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(t, 40, 0);
    }
    for (int k = 0; k < n; k++) {
        const vl_ac_t *ac = &a->ac[idx[k]];
        char name[24], sub[64], alt[24], dist[24], gs[24];
        if (ac->flight[0]) snprintf(name, sizeof name, "%s", ac->flight);
        else if (ac->reg[0]) snprintf(name, sizeof name, "%s", ac->reg);
        else snprintf(name, sizeof name, "%06X", (unsigned)(ac->hex & 0xFFFFFF));
        vl_fmt_alt(a, ac, alt, sizeof alt);
        vl_fmt_dist(a, ac->dist_km, dist, sizeof dist);
        vl_fmt_speed(a, ac, gs, sizeof gs);
        snprintf(sub, sizeof sub, "%s%s%s%s%s", ac->type[0] ? ac->type : "", ac->type[0] ? " · " : "",
                 ac->flight[0] && ac->reg[0] ? ac->reg : "", ac->flight[0] && ac->reg[0] ? " · " : "", gs);
        char val[56];
        snprintf(val, sizeof val, "%s\n%s", alt, dist);
        lv_obj_t *r = row(l, w, NULL, vl_alt_color(ac), name, sub, val, pick_cb, a, idx[k]);
        if (ac->has_sq && (ac->sq == 7500 || ac->sq == 7600 || ac->sq == 7700)) {
            lv_obj_set_style_border_color(r, AOS_C_RED, 0);
            lv_obj_set_style_border_width(r, 3, 0);
        }
    }
    lv_obj_update_layout(l);
    lv_obj_scroll_to_y(l, sy, LV_ANIM_OFF);
}

static void sort_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->list_by_alt = !a->list_by_alt;
    lv_obj_t *b = lv_event_get_current_target(e);
    lv_obj_t *t = lv_obj_get_child(b, 0);
    if (t) lv_label_set_text(t, a->list_by_alt ? _("Por altitud") : _("Por distancia"));
    if (a->list_box) lv_obj_scroll_to_y(a->list_box, 0, LV_ANIM_OFF);
    list_fill(a);
}

void vl_show_list(app_t *a)
{
    lv_obj_t *p = panel_new(a, SCR_LIST);
    lv_obj_t *back = round_btn(p, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, PAD, PAD);
    lv_obj_t *title = aos_label(p, _("Aviones"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 104, PAD + 18);

    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, sort_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *bt = aos_label(b, a->list_by_alt ? _("Por altitud") : _("Por distancia"), aos_font_body, AOS_C_TEXT);
    lv_obj_center(bt);
    lv_obj_remove_flag(bt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -PAD, PAD + 8);

    a->list_box = column(a, p, PAD + 100);
    a->list_ms = (uint32_t)aos_hal_uptime_ms();
    list_fill(a);
}

/* ---------------------------------------------------------------------------
 * The settings
 * ------------------------------------------------------------------------- */

enum {
    SET_SRC = 0, SET_RADIUS, SET_TRAIL, SET_UNITS, SET_LABELS, SET_ROUTES, SET_OVER, SET_EMERG, SET_MIL, SET_SOUND,
    SET_HERE, SET_CLIMA,
};

static const char *yes_no(bool b)
{
    return b ? _("sí") : _("no");
}

static void set_value(app_t *a, int what, char *out, size_t n)
{
    const vl_conf_t *c = &a->conf;
    char d[24];
    switch (what) {
    case SET_SRC: snprintf(out, n, "%s", c->src == SRC_LOCAL ? _("receptor propio") : vl_src_name(c->src)); break;
    case SET_RADIUS: vl_fmt_dist(a, (float)c->radius_km, d, sizeof d); snprintf(out, n, "%s", d); break;
    case SET_TRAIL:
        if (c->trail_min) snprintf(out, n, "%d min", c->trail_min);
        else snprintf(out, n, "%s", _("no"));
        break;
    case SET_UNITS: snprintf(out, n, "%s", c->units == UNITS_METRIC ? _("métricas") : _("de aviación")); break;
    case SET_LABELS: snprintf(out, n, "%s", yes_no(c->labels)); break;
    case SET_ROUTES: snprintf(out, n, "%s", yes_no(c->routes)); break;
    case SET_OVER: snprintf(out, n, "%s", yes_no(c->alert_over)); break;
    case SET_EMERG: snprintf(out, n, "%s", yes_no(c->alert_emerg)); break;
    case SET_MIL: snprintf(out, n, "%s", yes_no(c->alert_mil)); break;
    case SET_SOUND: snprintf(out, n, "%s", yes_no(c->alert_sound)); break;
    default: out[0] = 0; break;
    }
}

/* what each source allows, in one line under the source */
static const char *src_note(const app_t *a, char *buf, size_t n)
{
    switch (a->conf.src) {
    case SRC_AIRPLANES: return _("Cada 5 s. Uso no comercial.");
    case SRC_ADSBLOL:
        snprintf(buf, n, _("Cada %d s: %d pedidos por día."), vl_feed_interval_s(a), a->conf.lol_day);
        return buf;
    case SRC_OPENSKY:
        snprintf(buf, n, a->conf.os_id[0] ? _("Cada %d s, con tu cliente de la API.") : _("Cada %d s, sin cuenta."),
                 vl_feed_interval_s(a));
        return buf;
    default: return a->conf.url[0] ? a->conf.url : _("La dirección se escribe en la página del portal.");
    }
}

static void settings_build(app_t *a);

static void set_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int what = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    vl_conf_t *c = &a->conf;
    bool restart = false;
    switch (what) {
    case SET_SRC: c->src = (c->src + 1) % SRC_N; restart = true; break;
    case SET_RADIUS: {
        /* round numbers in the units shown: NM or km */
        static const int RN[] = { 15, 25, 50, 100, 150, 200 }, RK[] = { 25, 50, 100, 150, 250, 400 };
        const int *R = c->units == UNITS_METRIC ? RK : RN;
        float f = c->units == UNITS_METRIC ? 1.0f : 1.852f;
        int k = 0;
        while (k < 6 && (int)roundf(R[k] * f) <= c->radius_km) k++;
        c->radius_km = (int)roundf(R[k % 6] * f);
        restart = true;
        break;
    }
    case SET_TRAIL: {
        static const int T[] = { 0, 1, 3, 5, 10 };
        int k = 0;
        while (k < 5 && T[k] <= c->trail_min) k++;
        c->trail_min = T[k % 5];
        break;
    }
    case SET_UNITS: c->units = c->units == UNITS_METRIC ? UNITS_AVIATION : UNITS_METRIC; break;
    case SET_LABELS: c->labels = !c->labels; break;
    case SET_ROUTES: c->routes = !c->routes; break;
    case SET_OVER: c->alert_over = !c->alert_over; break;
    case SET_EMERG: c->alert_emerg = !c->alert_emerg; break;
    case SET_MIL: c->alert_mil = !c->alert_mil; break;
    case SET_SOUND: c->alert_sound = !c->alert_sound; break;
    case SET_HERE: {
        float lon, lat;
        vl_world_to_lonlat(a->view.cx, a->view.cy, &lon, &lat);
        c->lat = lat;
        c->lon = lon;
        c->have_place = true;
        snprintf(c->name, sizeof c->name, "%.4f, %.4f", (double)lat, (double)lon);
        vl_place_apply(a);
        restart = true;
        break;
    }
    case SET_CLIMA: {
        int32_t la, lo;
        if (!aos_hal_pref_get_i32("clima_lat", &la) || !aos_hal_pref_get_i32("clima_lon", &lo)) return;
        c->lat = (float)la / 1e4f;
        c->lon = (float)lo / 1e4f;
        c->have_place = true;
        c->name[0] = 0;
        aos_hal_pref_get_str("clima_city", c->name, sizeof c->name);
        vl_place_apply(a);
        vl_go_to(a, a->place_x, a->place_y, a->view.z);
        restart = true;
        break;
    }
    default: return;
    }
    vl_conf_save(a);
    if (restart) vl_feed_restart(a);
    if (what == SET_HERE || what == SET_CLIMA) {
        vl_show_map(a);
        return;
    }
    settings_build(a);
}

static void settings_build(app_t *a)
{
    lv_obj_t *l = a->list_box;
    if (!l) return;
    int32_t sy = lv_obj_get_scroll_y(l);
    lv_obj_clean(l);
    int w = col_w(a);
    char v[64], note[96];
    static const struct { int what; const char *sym; const char *name; } ROWS[] = {
        { SET_SRC, AOS_SYM_RADAR, N_("Fuente") },
        { SET_RADIUS, AOS_SYM_RULER, N_("Radio") },
        { SET_TRAIL, AOS_SYM_CHART_LINE, N_("Estelas") },
        { SET_UNITS, AOS_SYM_RULER, N_("Unidades") },
        { SET_LABELS, AOS_SYM_EYE_OUTLINE, N_("Nombres en el mapa") },
        { SET_ROUTES, AOS_SYM_EARTH, N_("Origen y destino") },
        { SET_OVER, AOS_SYM_BELL_OUTLINE, N_("Un avión pasa encima") },
        { SET_EMERG, AOS_SYM_ALERT_OUTLINE, N_("Emergencias") },
        { SET_MIL, AOS_SYM_BELL_OUTLINE, N_("Militares") },
        { SET_SOUND, AOS_SYM_VOLUME_HIGH, N_("Sonido de los avisos") },
    };
    heading(l, w, _("Datos"));
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        if (ROWS[i].what == SET_LABELS) heading(l, w, _("Mapa"));
        if (ROWS[i].what == SET_OVER) heading(l, w, _("Avisos"));
        set_value(a, ROWS[i].what, v, sizeof v);
        const char *sub = NULL;
        if (ROWS[i].what == SET_SRC) sub = src_note(a, note, sizeof note);
        if (ROWS[i].what == SET_OVER) {
            char dd[24];
            vl_fmt_dist(a, a->conf.over_km, dd, sizeof dd);
            snprintf(note, sizeof note, _("A menos de %s y %d ft"), dd, a->conf.over_ft);
            sub = note;
        }
        if (ROWS[i].what == SET_ROUTES) sub = _("Según adsbdb, por el número de vuelo");
        if (ROWS[i].what == SET_EMERG) sub = _("Squawk 7500, 7600 o 7700");
        if (ROWS[i].what == SET_MIL) sub = _("Si la fuente lo dice");
        row(l, w, ROWS[i].sym, 0, _(ROWS[i].name), sub, v, set_cb, a, ROWS[i].what);
    }

    heading(l, w, _("Lugar"));
    snprintf(note, sizeof note, "%s", a->conf.name[0] ? a->conf.name : "-");
    row(l, w, AOS_SYM_HOME_ASSISTANT, 0, _("Lugar elegido"), note, NULL, NULL, a, -1);
    row(l, w, AOS_SYM_RADAR, 0, _("Usar el centro del mapa"), NULL, NULL, set_cb, a, SET_HERE);
    int32_t la, lo;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        char city[48] = "";
        aos_hal_pref_get_str("clima_city", city, sizeof city);
        row(l, w, AOS_SYM_WEATHER_PARTLY_CLOUDY, 0, _("Ciudad de Clima"), city[0] ? city : NULL, NULL, set_cb, a,
            SET_CLIMA);
    }

    char info[400];
    snprintf(info, sizeof info, "%s\n%s\n%s %u KB, %d %s",
             _("Una pulsación larga en el mapa también elige el lugar. La dirección del receptor, las claves de los servicios y un lugar por su nombre se escriben en la página Vuelos del portal."),
             _("Mapa: OpenFreeMap, OpenMapTiles, OpenStreetMap. Aeropuertos: OurAirports."),
             _("Última lectura:"), (unsigned)(a->feed_bytes / 1024), a->nap, _("aeropuertos en la tarjeta"));
    lv_obj_t *t = aos_label(l, info, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(t, 18, 0);
    lv_obj_update_layout(l);
    lv_obj_scroll_to_y(l, sy, LV_ANIM_OFF);
}

void vl_show_settings(app_t *a)
{
    lv_obj_t *p = panel_new(a, SCR_SETTINGS);
    lv_obj_t *back = round_btn(p, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, PAD, PAD);
    lv_obj_t *title = aos_label(p, _("Ajustes"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 104, PAD + 18);
    a->list_box = column(a, p, PAD + 100);
    settings_build(a);
}

void vl_ui_relayout(app_t *a)
{
    if (a->screen == SCR_LIST) vl_show_list(a);
    else if (a->screen == SCR_SETTINGS) vl_show_settings(a);
}

void vl_ui_tick(app_t *a, uint32_t now)
{
    if (a->screen == SCR_LIST && now - a->list_ms > 3000) {
        a->list_ms = now;
        list_fill(a);
    }
}
