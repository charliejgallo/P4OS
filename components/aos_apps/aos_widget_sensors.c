/*
 * P4OS - the home screen's sensor widget (2026-10-25): menu.txt's
 * "widget sensors 4x2 [arg]", added from the portal's #inicio page.
 *
 * Up to six of the sensor service's values (aos_sensors.h), each with its
 * glyph, the value with its unit as Módulos shows it (aos_sensor_text.h),
 * and the quantity and the sensor it comes from. Without an arg it picks
 * on its own, what a home wants first: CO2, temperature, humidity,
 * pressure, particles, light, power; the motion sensors' axes and raw
 * counts never. With one it shows those: "name:key,name:key" (the sensor's
 * name as Módulos lists it, the value's key: "max6675:temp,scd41:co2"),
 * picked on the board in the home's edit mode from choices() below, or
 * typed in the portal.
 * CO2 and particles take a colour by their level. Two seconds a refresh,
 * redrawn only when the service read something new; a tap opens Módulos.
 * Nothing in internal RAM but the few words LVGL's labels take.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_sensor_text.h"
#include "aos_sensors.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CELLS 6

typedef struct {
    char name[24], key[12];             /* what this cell shows */
} pick_t;

typedef struct {
    lv_obj_t   *glyph[CELLS], *val[CELLS], *cap[CELLS], *empty;
    pick_t      pick[CELLS];
    int         n, cells;
    bool        fixed;                  /* the arg chose them */
    uint32_t    seq;
    lv_timer_t *timer;
} wsens_t;

/* what a home wants first; the rest never on its own */
static const int PRIORITY[] = { AOS_SQ_CO2, AOS_SQ_TEMP, AOS_SQ_HUM, AOS_SQ_PRESS, AOS_SQ_PM, AOS_SQ_TVOC,
                                AOS_SQ_INDEX, AOS_SQ_LUX, AOS_SQ_UVI, AOS_SQ_POWER, AOS_SQ_CURR, AOS_SQ_VOLT,
                                AOS_SQ_DIST, AOS_SQ_PERCENT, AOS_SQ_DOSE, AOS_SQ_RAIN, AOS_SQ_SPEED };

static void choose_auto(wsens_t *w)
{
    w->n = 0;
    int ns = aos_sensor_count();
    for (size_t p = 0; p < sizeof PRIORITY / sizeof PRIORITY[0] && w->n < w->cells; p++) {
        for (int i = 0; i < ns && w->n < w->cells; i++) {
            aos_sensor_t s;
            aos_sensor_value_t v[AOS_SENSOR_XVALUES];
            if (!aos_sensor_at(i, &s)) continue;
            int nv = aos_sensor_values(i, v, AOS_SENSOR_XVALUES);
            for (int k = 0; k < nv && w->n < w->cells; k++) {
                if (v[k].q != PRIORITY[p]) continue;
                snprintf(w->pick[w->n].name, sizeof w->pick[0].name, "%.23s", s.name);
                snprintf(w->pick[w->n].key, sizeof w->pick[0].key, "%.11s", v[k].key);
                w->n++;
            }
        }
    }
}

/* "name:key,name:key" */
static void choose_arg(wsens_t *w, const char *arg)
{
    w->n = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", arg);
    for (char *tok = strtok(buf, ","); tok && w->n < w->cells; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        char *colon = strchr(tok, ':');
        if (!colon || colon == tok || !colon[1]) continue;
        *colon = 0;
        snprintf(w->pick[w->n].name, sizeof w->pick[0].name, "%s", tok);
        snprintf(w->pick[w->n].key, sizeof w->pick[0].key, "%s", colon + 1);
        w->n++;
    }
}

/* the quantity's colour; CO2 and particles by how good the air is */
static lv_color_t tint(int q, float v)
{
    switch (q) {
    case AOS_SQ_CO2: return lv_color_hex(isnan(v) || v < 800 ? 0x30D158 : v < 1200 ? 0xFFD60A : 0xFF453A);
    case AOS_SQ_PM:  return lv_color_hex(isnan(v) || v < 12 ? 0x30D158 : v < 35 ? 0xFFD60A : 0xFF453A);
    case AOS_SQ_TEMP: return lv_color_hex(0xFF9F0A);
    case AOS_SQ_HUM: return lv_color_hex(0x64D2FF);
    case AOS_SQ_PRESS: return lv_color_hex(0xBF5AF2);
    case AOS_SQ_LUX: case AOS_SQ_UVI: return lv_color_hex(0xFFD60A);
    case AOS_SQ_POWER: case AOS_SQ_CURR: case AOS_SQ_VOLT: return lv_color_hex(0x30D158);
    default: return lv_color_white();
    }
}

static void set_text(lv_obj_t *l, const char *t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

/* the cell's value: the sensor by name, its value by key */
static bool lookup(const pick_t *p, aos_sensor_value_t *out)
{
    int ns = aos_sensor_count();
    for (int i = 0; i < ns; i++) {
        aos_sensor_t s;
        /* by its name (Módulos' list) or its chip's, case aside: "scd41:co2" */
        if (!aos_sensor_at(i, &s) || (strcasecmp(s.name, p->name) && strcasecmp(s.chip, p->name))) continue;
        aos_sensor_value_t v[AOS_SENSOR_XVALUES];
        int nv = aos_sensor_values(i, v, AOS_SENSOR_XVALUES);
        for (int k = 0; k < nv; k++) {
            if (strcmp(v[k].key, p->key)) continue;
            *out = v[k];
            if (s.state != AOS_SENSOR_OK) out->v = NAN;      /* failing: "--", not a stale number */
            return true;
        }
    }
    return false;
}

static void refresh(wsens_t *w)
{
    uint32_t seq = aos_sensors_seq();
    if (seq == w->seq && w->seq) return;
    w->seq = seq;
    /* chosen again on every new round: a sensor's first value can come a
     * few seconds after the sensor (an SCD41's CO2 takes five) */
    if (!w->fixed) choose_auto(w);
    lv_obj_set_flag(w->empty, LV_OBJ_FLAG_HIDDEN, w->n > 0);
    for (int c = 0; c < w->cells; c++) {
        bool on = c < w->n;
        lv_obj_set_flag(w->glyph[c], LV_OBJ_FLAG_HIDDEN, !on);
        lv_obj_set_flag(w->val[c], LV_OBJ_FLAG_HIDDEN, !on);
        lv_obj_set_flag(w->cap[c], LV_OBJ_FLAG_HIDDEN, !on);
        if (!on) continue;
        aos_sensor_value_t v = { 0 };
        bool have = lookup(&w->pick[c], &v);
        float x = have ? v.v : NAN;
        char t[48];
        aos_sensor_fmt(have ? v.q : AOS_SQ_RAW, x, t, sizeof t);
        set_text(w->val[c], t);
        set_text(w->glyph[c], aos_sensor_glyph(have ? v.q : AOS_SQ_RAW, w->pick[c].key));
        lv_obj_set_style_text_color(w->glyph[c], tint(have ? v.q : -1, x), 0);
        snprintf(t, sizeof t, "%s · %s", aos_sensor_key_name(w->pick[c].key), w->pick[c].name);
        set_text(w->cap[c], t);
    }
}

static void timer_cb(lv_timer_t *t) { refresh(lv_timer_get_user_data(t)); }
static void open_cb(lv_event_t *e) { (void)e; aos_ui_open("aos.modules"); }

static void deleted_cb(lv_event_t *e)
{
    wsens_t *w = lv_event_get_user_data(e);
    lv_timer_delete(w->timer);
    free(w);
}

static void widget_create(lv_obj_t *card, int32_t cw, int32_t ch, const char *arg)
{
    wsens_t *w = calloc(1, sizeof *w);
    if (!w) return;
    aos_sensors_start();                /* a widget asked for them: the service runs */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, open_cb, LV_EVENT_SHORT_CLICKED, NULL);

    int32_t iw = cw - 44, ih = ch - 44;
    lv_obj_t *hg = aos_label(card, AOS_SYM_THERMOMETER, &aos_sym_28, lv_color_white());
    lv_obj_align(hg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *ht = aos_label(card, _("Sensores"), aos_font_small, lv_color_white());
    lv_obj_align(ht, LV_ALIGN_TOP_LEFT, 40, 0);
    aos_make_decorative(hg);
    aos_make_decorative(ht);

    /* the cells: two columns when it is wide, as many rows as fit */
    const int32_t top = 48, cell_h = 112;
    int cols = iw >= 440 ? 2 : 1;
    int rows = (ih - top) / cell_h;
    if (rows < 1) rows = 1;
    w->cells = cols * rows > CELLS ? CELLS : cols * rows;
    int32_t col_w = iw / cols;
    for (int c = 0; c < w->cells; c++) {
        int32_t x = (c % cols) * col_w, y = top + (c / cols) * cell_h;
        w->glyph[c] = aos_label(card, "", &aos_sym_28, lv_color_white());
        lv_obj_set_pos(w->glyph[c], x, y + 8);
        w->val[c] = aos_label(card, "--", aos_font_title, lv_color_white());
        lv_obj_set_pos(w->val[c], x + 40, y);
        w->cap[c] = aos_label(card, "", aos_font_caption, lv_color_hex(0xDDE6F0));
        lv_obj_set_width(w->cap[c], col_w - 44);
        lv_label_set_long_mode(w->cap[c], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(w->cap[c], x + 40, y + 50);
        aos_make_decorative(w->glyph[c]);
        aos_make_decorative(w->val[c]);
        aos_make_decorative(w->cap[c]);
    }
    w->empty = aos_label(card, _("Todavía no hay sensores. Conectá uno al header o agregalo en Módulos."),
                         aos_font_caption, lv_color_hex(0xDDE6F0));
    lv_obj_set_width(w->empty, iw);
    lv_label_set_long_mode(w->empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(w->empty, 0, top);
    aos_make_decorative(w->empty);

    if (arg && arg[0]) {
        w->fixed = true;
        choose_arg(w, arg);
    }
    w->timer = lv_timer_create(timer_cb, 2000, w);
    lv_obj_add_event_cb(card, deleted_cb, LV_EVENT_DELETE, w);
    refresh(w);
}

/* For the edit mode's sheet: every value of every sensor, with what it reads now */
static int choices(aos_widget_choice_t *out, int max)
{
    int n = 0, ns = aos_sensor_count();
    for (int i = 0; i < ns && n < max; i++) {
        aos_sensor_t s;
        aos_sensor_value_t v[AOS_SENSOR_XVALUES];
        if (!aos_sensor_at(i, &s)) continue;
        int nv = aos_sensor_values(i, v, AOS_SENSOR_XVALUES);
        for (int k = 0; k < nv && n < max; k++) {
            aos_widget_choice_t *c = &out[n++];
            snprintf(c->value, sizeof c->value, "%.23s:%.11s", s.name, v[k].key);
            snprintf(c->label, sizeof c->label, "%.24s · %.23s", aos_sensor_key_name(v[k].key), s.name);
            aos_sensor_fmt(v[k].q, s.state == AOS_SENSOR_OK ? v[k].v : NAN, c->detail, sizeof c->detail);
        }
    }
    return n;
}

/* The lock screen's widget: the first two quantities a home wants (the
 * card's order), "650 ppm · 22,5 °C", with the first one's glyph. */
static bool lock_fill(char *text, size_t n, const char **glyph, uint32_t *rgb)
{
    int ns = aos_sensor_count(), got = 0;
    text[0] = '\0';
    /* one value of each quantity: two thermometers make temperature once */
    for (size_t p = 0; p < sizeof PRIORITY / sizeof PRIORITY[0] && got < 2; p++) {
        int before = got;
        for (int i = 0; i < ns && got == before; i++) {
            aos_sensor_value_t v[AOS_SENSOR_XVALUES];
            int nv = aos_sensor_values(i, v, AOS_SENSOR_XVALUES);
            for (int k = 0; k < nv && got == before; k++) {
                if (v[k].q != PRIORITY[p] || isnan(v[k].v)) continue;
                char one[32];
                aos_sensor_fmt(v[k].q, v[k].v, one, sizeof one);
                size_t at = strlen(text);
                snprintf(text + at, n - at, "%s%s", got ? "  ·  " : "", one);
                if (!got) {
                    *glyph = aos_sensor_glyph(v[k].q, v[k].key);
                    *rgb = lv_color_to_u32(tint(v[k].q, v[k].v)) & 0xFFFFFF;
                }
                got++;
            }
        }
    }
    return got > 0;
}

void aos_widget_sensors_register(void)
{
    aos_ui_register_lock_widget("sensors", N_("Sensores"), AOS_SYM_THERMOMETER, lock_fill);
    aos_ui_register_widget("sensors", widget_create);
    aos_ui_describe_widget("sensors", N_("Sensores"), N_("Lo que miden los sensores conectados: CO₂, temperatura, humedad…"),
                           AOS_SYM_THERMOMETER, CELLS, choices);
}
