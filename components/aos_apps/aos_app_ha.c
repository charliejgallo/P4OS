/*
 * P4OS - Home Assistant: the house on the bench's screen.
 *
 * Laid out like the phone's Home app. Across the top, the rooms (HA's
 * areas), plus Favoritos and Otros; in landscape they become a sidebar. Each
 * room shows two groups:
 *
 *   controls  lights, switches, fans, covers, climates, locks, media, scenes,
 *             scripts, buttons - one tile each. A tap does the obvious
 *             (toggle, open/close, run, play/pause; a lock asks first). A
 *             long press opens the tile's sheet: brightness, position,
 *             temperature and mode, and the star that makes it a favourite.
 *   sensors   compact tiles with the value, live.
 *
 * Everything comes from the service in aos_ha.c, which owns the connection;
 * this file only draws the table and queues calls. A tile answers the finger
 * at once - the service flips the state it expects - and HA's answer then
 * confirms or corrects it.
 *
 * The same file gives the home screen its "ha" widget: the first
 * favourites, tappable, and the house's name.
 */
#include "aos_apps.h"
#include "aos_ha.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_HA        lv_color_hex(0x41BDF5)
#define TAB_FAVS    (-2)
#define TAB_OTHER   (-1)
#define MAX_TILES   96
#define SIDEBAR_W   380

static struct {
    int tab;
    char sheet[64];                 /* the entity whose sheet is open, survives a turn */
    int hist_hours;                 /* the chart's period */
} S = { .tab = TAB_FAVS, .hist_hours = 24 };

typedef struct {
    char id[64];
    lv_obj_t *tile, *disc, *glyph, *name, *state, *value;
    bool sensor;
} tile_t;

static struct {
    lv_obj_t *root, *page, *overlay, *kb, *ta;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    uint32_t seen, sig;
    tile_t tiles[MAX_TILES];
    int ntiles;
    /* the open sheet */
    lv_obj_t *sh_slider, *sh_value, *sh_state, *sh_star, *sh_power;
    lv_obj_t *sh_chart, *sh_hi, *sh_lo, *sh_span, *sh_hint, *sh_ranges;
    lv_chart_series_t *sh_ser;
    bool sh_drag;
    int sh_kind;
    float sh_target;                /* climate: the target shown while tapping +/- */
    uint32_t sh_target_ms;
} U;

static void build(void);
static void sheet_open(const char *id);
static void hist_draw(void);

/* -------------------------------------------------------------------------- */
/* What an entity looks like                                                   */
/* -------------------------------------------------------------------------- */

static bool is_sensor(const aos_ha_entity_t *e) { return e->domain == HA_SENSOR || e->domain == HA_BINARY_SENSOR; }
static bool unavailable(const aos_ha_entity_t *e) { return !strcmp(e->state, "unavailable"); }

static bool is_on(const aos_ha_entity_t *e)
{
    const char *s = e->state;
    switch (e->domain) {
    case HA_COVER: return !strcmp(s, "open") || !strcmp(s, "opening") || !strcmp(s, "closing");
    case HA_CLIMATE: return strcmp(s, "off") && strcmp(s, "unavailable");
    case HA_LOCK: return !strcmp(s, "unlocked") || !strcmp(s, "unlocking");
    case HA_MEDIA: return !strcmp(s, "playing") || !strcmp(s, "on");
    case HA_SCENE: case HA_BUTTON: return false;
    default: return !strcmp(s, "on");
    }
}

static const char *glyph_of(const aos_ha_entity_t *e)
{
    bool on = is_on(e);
    const char *dc = e->device_class;
    switch (e->domain) {
    case HA_LIGHT: return on ? AOS_SYM_LIGHTBULB : AOS_SYM_LIGHTBULB_OUTLINE;
    case HA_SWITCH: return !strcmp(dc, "outlet") ? (on ? AOS_SYM_POWER_PLUG : AOS_SYM_POWER_PLUG_OFF) : AOS_SYM_TOGGLE_SWITCH;
    case HA_INPUT_BOOLEAN: return AOS_SYM_TOGGLE_SWITCH;
    case HA_FAN: return on ? AOS_SYM_FAN : AOS_SYM_FAN_OFF;
    case HA_COVER: return !strcmp(dc, "garage") ? (on ? AOS_SYM_GARAGE_OPEN : AOS_SYM_GARAGE) : (on ? AOS_SYM_BLINDS_OPEN : AOS_SYM_BLINDS);
    case HA_CLIMATE:
        return !strcmp(e->hvac_action, "cooling") ? AOS_SYM_SNOWFLAKE : !strcmp(e->hvac_action, "heating") ? AOS_SYM_FIRE
             : AOS_SYM_AIR_CONDITIONER;
    case HA_LOCK: return on ? AOS_SYM_LOCK_OPEN_VARIANT : AOS_SYM_LOCK;
    case HA_MEDIA: return AOS_SYM_TELEVISION;
    case HA_SCENE: return AOS_SYM_PALETTE;
    case HA_SCRIPT: return AOS_SYM_SCRIPT_TEXT;
    case HA_BUTTON: return AOS_SYM_GESTURE_TAP_BUTTON;
    case HA_SENSOR:
        if (!strcmp(dc, "temperature")) return AOS_SYM_THERMOMETER;
        if (!strcmp(dc, "humidity")) return AOS_SYM_WATER_PERCENT;
        if (!strcmp(dc, "power") || !strcmp(dc, "energy")) return AOS_SYM_LIGHTNING_BOLT;
        if (!strcmp(dc, "voltage") || !strcmp(dc, "current")) return AOS_SYM_FLASH;
        if (!strcmp(dc, "carbon_dioxide")) return AOS_SYM_MOLECULE_CO2;
        return AOS_SYM_GAUGE;
    case HA_BINARY_SENSOR:
        if (!strcmp(dc, "door") || !strcmp(dc, "garage_door")) return on ? AOS_SYM_DOOR_OPEN : AOS_SYM_DOOR;
        if (!strcmp(dc, "window")) return on ? AOS_SYM_WINDOW_OPEN : AOS_SYM_WINDOW_CLOSED;
        if (!strcmp(dc, "motion") || !strcmp(dc, "occupancy")) return AOS_SYM_MOTION_SENSOR;
        return AOS_SYM_GAUGE;
    }
    return AOS_SYM_HOME_ASSISTANT;
}

static uint32_t tint_of(const aos_ha_entity_t *e)
{
    switch (e->domain) {
    case HA_LIGHT: return 0xFFC83D;
    case HA_SWITCH: case HA_INPUT_BOOLEAN: return 0x30D158;
    case HA_FAN: return 0x64D2FF;
    case HA_COVER: return 0x0A84FF;
    case HA_CLIMATE: return !strcmp(e->hvac_action, "heating") || !strcmp(e->state, "heat") ? 0xFF9F0A : 0x64D2FF;
    case HA_LOCK: return is_on(e) ? 0xFF453A : 0x30D158;
    case HA_MEDIA: return 0xFF375F;
    case HA_SCENE: case HA_SCRIPT: case HA_BUTTON: return 0xBF5AF2;
    case HA_BINARY_SENSOR: return is_on(e) ? 0xFF9F0A : 0x8E8E93;
    default: return 0x41BDF5;
    }
}

/* "23.4" -> "23,4", the unit after a space; not-a-number states as they are */
static void fmt_value(const aos_ha_entity_t *e, char *out, size_t n)
{
    char *end;
    double v = strtod(e->state, &end);
    if (end == e->state || *end) { snprintf(out, n, "%s", e->state); return; }
    const char *dot = strchr(e->state, '.');
    int dec = dot ? (int)strlen(dot + 1) : 0;
    if (dec > 2) dec = 2;
    char num[24];
    snprintf(num, sizeof num, "%.*f", dec, v);
    for (char *p = num; *p; p++) if (*p == '.') *p = ',';
    snprintf(out, n, "%s%s%s", num, e->unit[0] ? " " : "", e->unit);
}

static void fmt_temp(float t, char *out, size_t n)
{
    if (isnan(t)) { snprintf(out, n, "--"); return; }
    if (fabsf(t - roundf(t)) < 0.05f) snprintf(out, n, "%.0f°", t);
    else {
        snprintf(out, n, "%.1f°", t);
        for (char *p = out; *p; p++) if (*p == '.') *p = ',';
    }
}

static const char *hvac_mode_name(const char *m)
{
    if (!strcmp(m, "cool")) return _("Frío");
    if (!strcmp(m, "heat")) return _("Calor");
    if (!strcmp(m, "heat_cool")) return _("Frío/calor");
    if (!strcmp(m, "auto")) return _("Auto");
    if (!strcmp(m, "dry")) return _("Seco");
    if (!strcmp(m, "fan_only")) return _("Ventilación");
    if (!strcmp(m, "off")) return _("Apagado");
    return m;
}

static void state_text(const aos_ha_entity_t *e, char *out, size_t n)
{
    const char *s = e->state;
    if (unavailable(e)) { snprintf(out, n, "%s", _("No disponible")); return; }
    switch (e->domain) {
    case HA_LIGHT:
        if (!strcmp(s, "on") && e->brightness > 0)
            snprintf(out, n, "%s · %d %%", _("Encendida"), (e->brightness * 100 + 127) / 255);
        else snprintf(out, n, "%s", !strcmp(s, "on") ? _("Encendida") : _("Apagada"));
        return;
    case HA_SWITCH: case HA_INPUT_BOOLEAN:
        snprintf(out, n, "%s", !strcmp(s, "on") ? _("Encendido") : _("Apagado"));
        return;
    case HA_FAN:
        if (!strcmp(s, "on") && e->position > 0) snprintf(out, n, "%s · %d %%", _("Encendido"), e->position);
        else snprintf(out, n, "%s", !strcmp(s, "on") ? _("Encendido") : _("Apagado"));
        return;
    case HA_COVER:
        if (!strcmp(s, "opening")) snprintf(out, n, "%s", _("Abriendo…"));
        else if (!strcmp(s, "closing")) snprintf(out, n, "%s", _("Cerrando…"));
        else if (!strcmp(s, "open") && e->position >= 0 && e->position < 100)
            snprintf(out, n, "%s · %d %%", _("Abierta"), e->position);
        else snprintf(out, n, "%s", !strcmp(s, "open") ? _("Abierta") : _("Cerrada"));
        return;
    case HA_CLIMATE: {
        char cur[16], tgt[16];
        fmt_temp(e->current_temp, cur, sizeof cur);
        fmt_temp(e->target_temp, tgt, sizeof tgt);
        if (!strcmp(s, "off")) snprintf(out, n, "%s · %s", cur, _("Apagado"));
        else snprintf(out, n, "%s · %s %s", cur, hvac_mode_name(s), tgt);
        return;
    }
    case HA_LOCK:
        snprintf(out, n, "%s", !strcmp(s, "locked") ? _("Con llave") : !strcmp(s, "unlocked") ? _("Sin llave")
                             : !strcmp(s, "locking") ? _("Cerrando…") : !strcmp(s, "unlocking") ? _("Abriendo…") : s);
        return;
    case HA_MEDIA:
        snprintf(out, n, "%s", !strcmp(s, "playing") ? _("Reproduciendo") : !strcmp(s, "paused") ? _("En pausa")
                             : !strcmp(s, "off") ? _("Apagada") : !strcmp(s, "idle") ? _("Inactiva") : _("Encendida"));
        return;
    case HA_SCENE: snprintf(out, n, "%s", _("Escena")); return;
    case HA_SCRIPT: snprintf(out, n, "%s", !strcmp(s, "on") ? _("En marcha…") : _("Guion")); return;
    case HA_BUTTON: snprintf(out, n, "%s", _("Botón")); return;
    case HA_BINARY_SENSOR: {
        const char *dc = e->device_class;
        bool on = !strcmp(s, "on");
        if (!strcmp(dc, "door") || !strcmp(dc, "window") || !strcmp(dc, "garage_door") || !strcmp(dc, "opening"))
            snprintf(out, n, "%s", on ? _("Abierta") : _("Cerrada"));
        else if (!strcmp(dc, "motion") || !strcmp(dc, "occupancy"))
            snprintf(out, n, "%s", on ? _("Movimiento") : _("Sin movimiento"));
        else snprintf(out, n, "%s", on ? _("Activado") : _("Normal"));
        return;
    }
    default: fmt_value(e, out, n); return;
    }
}

/* -------------------------------------------------------------------------- */
/* Tiles                                                                       */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void tile_update(tile_t *t, const aos_ha_entity_t *e)
{
    char st[64];
    if (t->sensor) {
        char v[40];
        if (e->domain == HA_BINARY_SENSOR) state_text(e, v, sizeof v);
        else if (unavailable(e)) snprintf(v, sizeof v, "%s", _("No disponible"));
        else fmt_value(e, v, sizeof v);
        lv_label_set_text(t->value, v);
        lv_label_set_text(t->glyph, glyph_of(e));
        lv_obj_set_style_text_color(t->glyph, lv_color_hex(tint_of(e)), 0);
        lv_obj_set_style_opa(t->tile, unavailable(e) ? LV_OPA_50 : LV_OPA_COVER, 0);
        return;
    }
    bool on = is_on(e);
    state_text(e, st, sizeof st);
    lv_label_set_text(t->state, st);
    lv_label_set_text(t->glyph, glyph_of(e));
    uint32_t tint = tint_of(e);
    /* on: a light tile with a coloured glyph; off: a dark one, like the phone */
    lv_obj_set_style_bg_color(t->tile, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD, 0);
    lv_obj_set_style_text_color(t->name, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(t->state, on ? lv_color_hex(0x636366) : AOS_C_DIM, 0);
    lv_obj_set_style_bg_color(t->disc, on ? lv_color_hex(tint) : AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(t->glyph, on ? lv_color_white() : lv_color_hex(0xAEAEB2), 0);
    lv_obj_set_style_opa(t->tile, unavailable(e) ? LV_OPA_40 : e->pending ? LV_OPA_80 : LV_OPA_COVER, 0);
}

static void confirm_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.sh_slider = U.sh_value = U.sh_state = U.sh_star = U.sh_power = NULL;
    U.sh_chart = U.sh_hi = U.sh_lo = U.sh_span = U.sh_hint = U.sh_ranges = NULL;
    U.kb = U.ta = NULL;
    S.sheet[0] = 0;
}

static char s_confirm_id[64];

static void confirm_yes_cb(lv_event_t *e)
{
    aos_ha_tap(s_confirm_id);
    confirm_close();
}

static void confirm_no_cb(lv_event_t *e) { confirm_close(); }

static void confirm(const char *id, const char *question, const char *yes)
{
    confirm_close();
    snprintf(s_confirm_id, sizeof s_confirm_id, "%s", id);
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_60, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.overlay, confirm_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c = box(U.overlay, 560, LV_SIZE_CONTENT);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(c, 16, 0);
    lv_obj_center(c);
    lv_obj_t *q = aos_label(c, question, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(q, lv_pct(100));
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = box(c, 244, 88);
        lv_obj_set_style_radius(b, 44, 0);
        lv_obj_set_style_bg_color(b, i ? AOS_C_RED : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, i ? confirm_yes_cb : confirm_no_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(aos_label(b, i ? yes : _("Cancelar"), aos_font_body, AOS_C_TEXT));
    }
}

static void tile_click_cb(lv_event_t *ev)
{
    tile_t *t = lv_event_get_user_data(ev);
    aos_ha_lock();
    int i = aos_ha_find(t->id);
    aos_ha_entity_t e = i >= 0 ? *aos_ha_at(i) : (aos_ha_entity_t){ 0 };
    aos_ha_unlock();
    if (i < 0) return;
    if (is_sensor(&e) || e.domain == HA_CLIMATE) { sheet_open(t->id); return; }
    if (e.domain == HA_LOCK) {
        char q[96];
        bool locked = !strcmp(e.state, "locked");
        snprintf(q, sizeof q, locked ? _("¿Abrir %s?") : _("¿Cerrar %s con llave?"), e.name);
        confirm(t->id, q, locked ? _("Abrir") : _("Cerrar"));
        return;
    }
    if (!aos_ha_tap(t->id)) aos_ui_toast(_("Home Assistant no está conectado"), 1500);
}

static void tile_long_cb(lv_event_t *ev)
{
    tile_t *t = lv_event_get_user_data(ev);
    sheet_open(t->id);
    lv_indev_wait_release(lv_indev_active());       /* the release must not also be a click */
}

static lv_obj_t *tile_create(lv_obj_t *parent, tile_t *t, const aos_ha_entity_t *e, int32_t w)
{
    snprintf(t->id, sizeof t->id, "%s", e->id);
    t->sensor = is_sensor(e);
    int32_t h = t->sensor ? 112 : 164;
    lv_obj_t *c = box(parent, w, h);
    t->tile = c;
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_scale(c, 245, LV_STATE_PRESSED);    /* the tile gives a little under the finger */
    lv_obj_set_style_transform_pivot_x(c, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(c, h / 2, 0);
    lv_obj_add_event_cb(c, tile_click_cb, LV_EVENT_SHORT_CLICKED, t);
    lv_obj_add_event_cb(c, tile_long_cb, LV_EVENT_LONG_PRESSED, t);
    if (t->sensor) {
        t->glyph = aos_label(c, "", &aos_sym_28, AOS_C_DIM);
        lv_obj_align(t->glyph, LV_ALIGN_TOP_LEFT, 0, 0);
        t->name = aos_label(c, e->name, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(t->name, w - 36 - 40);
        lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t->name, LV_ALIGN_TOP_LEFT, 40, 2);
        t->value = aos_label(c, "", aos_font_title, AOS_C_TEXT);
        lv_obj_set_width(t->value, w - 36);
        lv_label_set_long_mode(t->value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t->value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
        t->disc = box(c, 64, 64);
        lv_obj_set_style_radius(t->disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(t->disc, LV_OPA_COVER, 0);
        lv_obj_align(t->disc, LV_ALIGN_TOP_LEFT, 0, 0);
        t->glyph = aos_label(t->disc, "", &aos_sym_44, AOS_C_TEXT);
        lv_obj_center(t->glyph);
        t->name = aos_label(c, e->name, aos_font_body, AOS_C_TEXT);
        lv_obj_set_size(t->name, w - 36, lv_font_get_line_height(aos_font_body));
        lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t->name, LV_ALIGN_BOTTOM_LEFT, 0, -30);
        t->state = aos_label(c, "", aos_font_caption, AOS_C_DIM);
        lv_obj_set_size(t->state, w - 36, lv_font_get_line_height(aos_font_caption));
        lv_label_set_long_mode(t->state, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t->state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    aos_make_decorative(t->glyph);
    tile_update(t, e);
    return c;
}

/* -------------------------------------------------------------------------- */
/* The sheet                                                                   */
/* -------------------------------------------------------------------------- */

enum { SK_NONE, SK_BRIGHT, SK_POS, SK_FAN, SK_CLIMATE, SK_SENSOR, SK_ACTION };

static void sheet_close_cb(lv_event_t *e) { confirm_close(); }

static void star_cb(lv_event_t *ev)
{
    bool on = !aos_ha_is_fav(S.sheet);
    aos_ha_set_fav(S.sheet, on);
    lv_label_set_text(lv_obj_get_child(U.sh_star, 0), on ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE);
    lv_obj_set_style_text_color(lv_obj_get_child(U.sh_star, 0), on ? AOS_C_YELLOW : AOS_C_DIM, 0);
    aos_ui_toast(on ? _("Agregado a favoritos") : _("Quitado de favoritos"), 1200);
}

static void slider_cb(lv_event_t *ev)
{
    lv_event_code_t c = lv_event_get_code(ev);
    int v = lv_slider_get_value(lv_event_get_target(ev));
    if (U.sh_value) lv_label_set_text_fmt(U.sh_value, "%d %%", v);
    if (c == LV_EVENT_PRESSED) U.sh_drag = true;
    if (c != LV_EVENT_RELEASED) return;
    U.sh_drag = false;
    char d[48];
    const char *dom = strchr(S.sheet, '.') ? S.sheet : "";
    if (U.sh_kind == SK_BRIGHT) {
        if (v <= 0) aos_ha_call("light", "turn_off", dom, NULL);
        else { snprintf(d, sizeof d, "{\"brightness_pct\":%d}", v); aos_ha_call("light", "turn_on", dom, d); }
    } else if (U.sh_kind == SK_POS) {
        snprintf(d, sizeof d, "{\"position\":%d}", v);
        aos_ha_call("cover", "set_cover_position", dom, d);
    } else if (U.sh_kind == SK_FAN) {
        snprintf(d, sizeof d, "{\"percentage\":%d}", v);
        aos_ha_call("fan", "set_percentage", dom, d);
    }
}

static void power_cb(lv_event_t *ev) { aos_ha_tap(S.sheet); }

static void cover_cb(lv_event_t *ev)
{
    static const char *const SVC[3] = { "open_cover", "stop_cover", "close_cover" };
    aos_ha_call("cover", SVC[(int)(intptr_t)lv_event_get_user_data(ev)], S.sheet, NULL);
}

static void temp_cb(lv_event_t *ev)
{
    aos_ha_lock();
    int i = aos_ha_find(S.sheet);
    aos_ha_entity_t e = i >= 0 ? *aos_ha_at(i) : (aos_ha_entity_t){ 0 };
    aos_ha_unlock();
    if (i < 0) return;
    float step = e.temp_step > 0 ? e.temp_step : 0.5f;
    float t = (U.sh_target_ms ? U.sh_target : e.target_temp) + (int)(intptr_t)lv_event_get_user_data(ev) * step;
    if (t < e.temp_min) t = e.temp_min;
    if (t > e.temp_max) t = e.temp_max;
    U.sh_target = t;
    U.sh_target_ms = (uint32_t)aos_hal_uptime_ms();       /* sent when the taps stop */
    char s[16];
    fmt_temp(t, s, sizeof s);
    lv_label_set_text(U.sh_value, s);
}

static void mode_cb(lv_event_t *ev)
{
    int m = (int)(intptr_t)lv_event_get_user_data(ev);
    char d[48];
    snprintf(d, sizeof d, "{\"hvac_mode\":\"%s\"}", aos_ha_hvac_names[m]);
    aos_ha_call("climate", "set_hvac_mode", S.sheet, d);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 96);
    lv_obj_set_style_min_width(b, 96, 0);
    lv_obj_set_style_radius(b, 48, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, text ? 30 : 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, AOS_C_TEXT));
    return b;
}

static lv_obj_t *big_slider(lv_obj_t *parent, int value, lv_color_t color, int32_t h)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, 200, h);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(s, 44, 0);
    lv_obj_set_style_radius(s, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, color, LV_PART_INDICATOR);
    lv_obj_set_style_clip_corner(s, true, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static void sheet_refresh(void)
{
    if (!U.overlay || !S.sheet[0] || U.sh_drag) return;
    aos_ha_lock();
    int i = aos_ha_find(S.sheet);
    aos_ha_entity_t e = i >= 0 ? *aos_ha_at(i) : (aos_ha_entity_t){ 0 };
    aos_ha_unlock();
    if (i < 0) return;
    char st[64];
    state_text(&e, st, sizeof st);
    aos_ha_lock();
    const char *room = e.area >= 0 ? aos_ha_area_name(e.area) : "";
    if (U.sh_state) {
        if (room[0]) lv_label_set_text_fmt(U.sh_state, "%s · %s", room, st);
        else lv_label_set_text(U.sh_state, st);
    }
    aos_ha_unlock();
    if (U.sh_power) lv_label_set_text(U.sh_power, is_on(&e) ? _("Apagar") : _("Encender"));
    if (U.sh_slider) {
        int v = U.sh_kind == SK_BRIGHT ? (e.brightness > 0 && is_on(&e) ? (e.brightness * 100 + 127) / 255 : 0) : e.position;
        if (v < 0) v = 0;
        lv_slider_set_value(U.sh_slider, v, LV_ANIM_ON);
        if (U.sh_value) lv_label_set_text_fmt(U.sh_value, "%d %%", v);
    }
    if (U.sh_chart) hist_draw();
    if (U.sh_kind == SK_SENSOR && U.sh_value) {
        char v[40];
        if (e.domain == HA_BINARY_SENSOR) state_text(&e, v, sizeof v);
        else fmt_value(&e, v, sizeof v);
        lv_label_set_text(U.sh_value, v);
    }
    if (U.sh_kind == SK_CLIMATE && U.sh_value && !U.sh_target_ms) {
        char v[16];
        fmt_temp(e.target_temp, v, sizeof v);
        lv_label_set_text(U.sh_value, v);
    }
}

/* ---- the history chart of a sensor ---- */

static const int HIST_HOURS[3] = { 6, 24, 24 * 7 };

static void hist_draw(void)
{
    if (!U.sh_chart || !S.sheet[0]) return;
    static float v[240];
    float lo = 0, hi = 0;
    uint32_t span = 0;
    int n = aos_ha_history(S.sheet, v, NULL, 240, &lo, &hi, &span);
    if (n <= 0) {
        lv_label_set_text(U.sh_hint, n == -AOS_HA_HIST_EMPTY ? _("Sin datos numéricos en este período.")
                                   : n == -AOS_HA_HIST_FAILED ? _("Home Assistant no dio el historial.")
                                   : !aos_hal_time_is_valid() ? _("Hace falta la hora para pedir el historial.")
                                   : _("Pidiendo el historial…"));
        lv_obj_remove_flag(U.sh_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.sh_chart, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(U.sh_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U.sh_chart, LV_OBJ_FLAG_HIDDEN);
    /* integer chart: the values scaled to two decimals, the range padded */
    float pad = (hi - lo) * 0.1f;
    if (pad < 0.05f) pad = 0.5f;
    lv_chart_set_point_count(U.sh_chart, (uint32_t)n);
    lv_chart_set_axis_range(U.sh_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lroundf((lo - pad) * 100), (int32_t)lroundf((hi + pad) * 100));
    for (int i = 0; i < n; i++) lv_chart_set_value_by_id(U.sh_chart, U.sh_ser, (uint32_t)i, (int32_t)lroundf(v[i] * 100));
    lv_chart_refresh(U.sh_chart);
    aos_ha_lock();
    int k = aos_ha_find(S.sheet);
    char unit[12] = "";
    if (k >= 0) snprintf(unit, sizeof unit, "%s", aos_ha_at(k)->unit);
    aos_ha_unlock();
    aos_ha_entity_t fake = { 0 };
    char t[40];
    snprintf(fake.unit, sizeof fake.unit, "%s", unit);
    snprintf(fake.state, sizeof fake.state, "%.1f", hi);
    fmt_value(&fake, t, sizeof t);
    lv_label_set_text_fmt(U.sh_hi, "%s %s", _("máx"), t);
    snprintf(fake.state, sizeof fake.state, "%.1f", lo);
    fmt_value(&fake, t, sizeof t);
    lv_label_set_text_fmt(U.sh_lo, "%s %s", _("mín"), t);
    uint32_t h = span / 3600;
    if (h >= 48) lv_label_set_text_fmt(U.sh_span, _("hace %u días  ·  ahora"), (unsigned)(h / 24));
    else lv_label_set_text_fmt(U.sh_span, _("hace %u h  ·  ahora"), (unsigned)h);
}

static void hist_range_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    S.hist_hours = HIST_HOURS[i];
    for (uint32_t k = 0; k < lv_obj_get_child_count(U.sh_ranges); k++) {
        lv_obj_t *c = lv_obj_get_child(U.sh_ranges, (int32_t)k);
        bool on = (int)k == i;
        lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(c, 0), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    aos_ha_history_request(S.sheet, S.hist_hours);
    hist_draw();
}

static void build_history(lv_obj_t *body, int32_t w, const aos_ha_entity_t *e)
{
    U.sh_ranges = box(body, LV_SIZE_CONTENT, 64);
    lv_obj_set_flex_flow(U.sh_ranges, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(U.sh_ranges, 10, 0);
    static const char *const NAMES[3] = { "6 h", "24 h", N_("7 días") };
    for (int i = 0; i < 3; i++) {
        bool on = S.hist_hours == HIST_HOURS[i];
        lv_obj_t *c = box(U.sh_ranges, LV_SIZE_CONTENT, 60);
        lv_obj_set_style_radius(c, 30, 0);
        lv_obj_set_style_pad_hor(c, 22, 0);
        lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, hist_range_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(c, aos_tr(NAMES[i]), aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    }
    lv_obj_t *frame = box(body, w, U.land ? 200 : 320);
    U.sh_hi = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.sh_hi, LV_ALIGN_TOP_LEFT, 0, 0);
    U.sh_lo = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.sh_lo, LV_ALIGN_BOTTOM_LEFT, 0, -30);
    U.sh_span = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.sh_span, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    U.sh_chart = lv_chart_create(frame);
    lv_obj_set_size(U.sh_chart, w, lv_obj_get_style_height(frame, 0) - 64);
    lv_obj_align(U.sh_chart, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_opa(U.sh_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(U.sh_chart, 0, 0);
    lv_obj_set_style_pad_all(U.sh_chart, 0, 0);
    lv_obj_set_style_line_color(U.sh_chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_div_line_count(U.sh_chart, 3, 0);
    lv_chart_set_type(U.sh_chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_size(U.sh_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.sh_chart, 4, LV_PART_ITEMS);
    U.sh_ser = lv_chart_add_series(U.sh_chart, lv_color_hex(tint_of(e)), LV_CHART_AXIS_PRIMARY_Y);
    U.sh_hint = aos_label(frame, "", aos_font_small, AOS_C_DIM);
    lv_obj_center(U.sh_hint);
    aos_ha_history_request(e->id, S.hist_hours);
    hist_draw();
}

static void sheet_open(const char *id)
{
    aos_ha_lock();
    int i = aos_ha_find(id);
    aos_ha_entity_t e = i >= 0 ? *aos_ha_at(i) : (aos_ha_entity_t){ 0 };
    aos_ha_unlock();
    if (i < 0) return;
    confirm_close();
    snprintf(S.sheet, sizeof S.sheet, "%s", id);
    U.sh_target_ms = 0;
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_70, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.overlay, sheet_close_cb, LV_EVENT_CLICKED, NULL);

    int32_t w = U.land ? 820 : U.W - 2 * AOS_UI_PAD, h = U.land ? U.H - 40 : U.H * 3 / 4;
    lv_obj_t *c = box(U.overlay, w, h);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x161618), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS + 8, 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_align(c, U.land ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, U.land ? 0 : -20);

    /* header: glyph, name, room; the star and the close button */
    lv_obj_t *disc = box(c, 80, 80);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(tint_of(&e)), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_center(aos_label(disc, glyph_of(&e), &aos_sym_44, lv_color_white()));
    lv_obj_t *nm = aos_label(c, e.name, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(nm, w - 56 - 100 - 200);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 100, 0);
    U.sh_state = aos_label(c, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(U.sh_state, w - 56 - 100 - 200);
    lv_label_set_long_mode(U.sh_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.sh_state, LV_ALIGN_TOP_LEFT, 100, 48);
    U.sh_star = box(c, 88, 88);
    lv_obj_add_flag(U.sh_star, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.sh_star, star_cb, LV_EVENT_CLICKED, NULL);
    bool fav = aos_ha_is_fav(id);
    lv_obj_center(aos_label(U.sh_star, fav ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE, &aos_sym_44, fav ? AOS_C_YELLOW : AOS_C_DIM));
    lv_obj_align(U.sh_star, LV_ALIGN_TOP_RIGHT, -96, -4);
    lv_obj_t *x = box(c, 72, 72);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(x, sheet_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(aos_label(x, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT));
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 4);

    /* the body */
    lv_obj_t *body = box(c, w - 56, h - 56 - 120);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 24, 0);
    /* the slider takes what is left after its value, its buttons and the gaps */
    int32_t sh = (h - 56 - 120) - 70 - 96 - 2 * 24 - 8;
    U.sh_kind = SK_ACTION;
    if (e.domain == HA_LIGHT && e.brightness >= 0) U.sh_kind = SK_BRIGHT;
    else if (e.domain == HA_COVER && e.position >= 0) U.sh_kind = SK_POS;
    else if (e.domain == HA_FAN && e.position >= 0) U.sh_kind = SK_FAN;
    else if (e.domain == HA_CLIMATE) U.sh_kind = SK_CLIMATE;
    else if (is_sensor(&e)) U.sh_kind = SK_SENSOR;

    if (U.sh_kind == SK_BRIGHT || U.sh_kind == SK_POS || U.sh_kind == SK_FAN) {
        U.sh_value = aos_label(body, "", aos_font_large, AOS_C_TEXT);
        U.sh_slider = big_slider(body, 0, lv_color_hex(tint_of(&e)), sh);
        lv_obj_t *row = box(body, LV_SIZE_CONTENT, 96);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 16, 0);
        if (U.sh_kind == SK_POS) {
            round_btn(row, AOS_SYM_CHEVRON_UP, _("Abrir"), AOS_C_CARD2, cover_cb, (void *)0);
            round_btn(row, AOS_SYM_STOP, NULL, AOS_C_CARD2, cover_cb, (void *)1);
            round_btn(row, AOS_SYM_CHEVRON_DOWN, _("Cerrar"), AOS_C_CARD2, cover_cb, (void *)2);
        } else {
            lv_obj_t *pb = round_btn(row, AOS_SYM_POWER, is_on(&e) ? _("Apagar") : _("Encender"), AOS_C_CARD2, power_cb, NULL);
            U.sh_power = lv_obj_get_child(pb, 1);
        }
    } else if (U.sh_kind == SK_CLIMATE) {
        char cur[16];
        fmt_temp(e.current_temp, cur, sizeof cur);
        lv_obj_t *l = aos_label(body, "", aos_font_body, AOS_C_DIM);
        lv_label_set_text_fmt(l, "%s %s", _("Ahora"), cur);
        lv_obj_t *row = box(body, LV_SIZE_CONTENT, 200);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 40, 0);
        round_btn(row, AOS_SYM_MINUS, NULL, AOS_C_CARD2, temp_cb, (void *)-1);
        U.sh_value = aos_label(row, "", &aos_inter_num_144, lv_color_hex(tint_of(&e)));
        lv_obj_set_style_min_width(U.sh_value, 300, 0);
        lv_obj_set_style_text_align(U.sh_value, LV_TEXT_ALIGN_CENTER, 0);
        round_btn(row, AOS_SYM_PLUS, NULL, AOS_C_CARD2, temp_cb, (void *)1);
        lv_obj_t *modes = box(body, w - 56, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(modes, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(modes, 12, 0);
        for (int m = 0; m < AOS_HA_HVAC_COUNT; m++) {
            if (!(e.hvac_modes & (1u << m))) continue;
            bool cur_m = !strcmp(e.state, aos_ha_hvac_names[m]);
            round_btn(modes, NULL, hvac_mode_name(aos_ha_hvac_names[m]), cur_m ? lv_color_hex(tint_of(&e)) : AOS_C_CARD2,
                      mode_cb, (void *)(intptr_t)m);
        }
    } else if (U.sh_kind == SK_SENSOR) {
        U.sh_value = aos_label(body, "", aos_font_huge, lv_color_hex(tint_of(&e)));
        uint32_t ago = ((uint32_t)aos_hal_uptime_ms() - e.changed_ms) / 1000;
        lv_obj_t *l = aos_label(body, "", aos_font_small, AOS_C_DIM);
        if (ago < 60) lv_label_set_text(l, _("Actualizado recién"));
        else lv_label_set_text_fmt(l, _("Actualizado hace %u min"), (unsigned)(ago / 60));
        if (e.domain == HA_SENSOR) build_history(body, w - 56, &e);
    } else {
        const char *verb = e.domain == HA_SCENE ? _("Activar") : e.domain == HA_SCRIPT ? _("Ejecutar")
                         : e.domain == HA_BUTTON ? _("Apretar") : e.domain == HA_MEDIA ? _("Reproducir / pausar")
                         : e.domain == HA_LOCK ? (strcmp(e.state, "locked") ? _("Cerrar con llave") : _("Abrir"))
                         : is_on(&e) ? _("Apagar") : _("Encender");
        round_btn(body, e.domain == HA_MEDIA ? AOS_SYM_PLAY : AOS_SYM_POWER, verb, lv_color_hex(tint_of(&e)), power_cb, NULL);
    }
    sheet_refresh();
}

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

static void url_ready_cb(lv_event_t *ev)
{
    lv_event_code_t c = lv_event_get_code(ev);
    if (c == LV_EVENT_READY) {
        if (!aos_ha_set_url(lv_textarea_get_text(U.ta))) aos_ui_toast(_("La dirección empieza con http://"), 2000);
        confirm_close();
        build();
    } else if (c == LV_EVENT_CANCEL) {
        confirm_close();
        build();
    }
}

static void url_edit_cb(lv_event_t *ev)
{
    confirm_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = aos_label(U.overlay, _("Dirección de Home Assistant"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 30);
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_text(U.ta, aos_ha_url()[0] ? aos_ha_url() : "http://");
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 100);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    lv_obj_t *n = aos_label(U.overlay, _("Por ejemplo http://192.168.1.10:8123, o https://… si entrás con certificado (Nabu Casa)"),
                            aos_font_caption, AOS_C_DIM);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 200);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, url_ready_cb, LV_EVENT_ALL, NULL);
}

static void import_cb(lv_event_t *ev)
{
    if (aos_ha_import_file()) aos_ui_toast(_("Leí ha.txt"), 1500);
    else aos_ui_toast(_("No hay nada nuevo en ha.txt"), 2000);
    confirm_close();
    build();
}

static void reconnect_cb(lv_event_t *ev) { aos_ha_reconnect(); confirm_close(); build(); }

static lv_obj_t *cfg_row(lv_obj_t *g, const char *label, const char *value, lv_color_t vc, lv_event_cb_t cb)
{
    lv_obj_t *r = box(g, lv_pct(100), AOS_UI_ROW_H);
    lv_obj_set_style_pad_hor(r, 22, 0);
    if (lv_obj_get_child_count(g) > 1) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    lv_obj_align(aos_label(r, label, aos_font_body, cb && !value ? AOS_C_ACCENT : AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    if (value) {
        lv_obj_t *v = aos_label(r, value, aos_font_body, vc);
        lv_obj_set_width(v, lv_pct(60));
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    }
    return r;
}

static const char *state_name(aos_ha_state_t s)
{
    switch (s) {
    case AOS_HA_UNCONFIGURED: return _("Sin configurar");
    case AOS_HA_WAITING_NET: return _("Esperando el Wi-Fi");
    case AOS_HA_CONNECTING: return _("Conectando…");
    case AOS_HA_AUTH_FAILED: return _("Token rechazado");
    case AOS_HA_LOADING: return _("Cargando la casa…");
    case AOS_HA_READY: return _("Conectado");
    default: return _("Sin conexión");
    }
}

static void config_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *g = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    cfg_row(g, _("Dirección"), aos_ha_url()[0] ? aos_ha_url() : _("tocá para escribirla"), AOS_C_DIM, url_edit_cb);
    cfg_row(g, _("Token"), aos_ha_has_token() ? _("guardado") : _("falta"), aos_ha_has_token() ? AOS_C_GREEN : AOS_C_ORANGE, NULL);
    aos_ha_state_t st = aos_ha_state();
    cfg_row(g, _("Estado"), state_name(st), st == AOS_HA_READY ? AOS_C_GREEN : AOS_C_DIM, NULL);
    lv_obj_t *g2 = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g2, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(g2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g2, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(g2, LV_FLEX_FLOW_COLUMN);
    cfg_row(g2, _("Leer ha.txt de la tarjeta"), NULL, AOS_C_ACCENT, import_cb);
    cfg_row(g2, _("Reconectar"), NULL, AOS_C_ACCENT, reconnect_cb);
    const char *err = aos_ha_error();
    if (err[0] && st != AOS_HA_READY) {
        lv_obj_t *l = aos_label(parent, err, aos_font_small, AOS_C_ORANGE);
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_t *n = aos_label(parent,
        _("El token se crea en Home Assistant: tu perfil > Seguridad > Tokens de acceso de larga duración. "
          "Escribirlo acá es un suplicio, así que va en un archivo ha.txt en la raíz de la tarjeta, con dos "
          "renglones: url=http://… (o https://…) y token=…. Al leerlo, P4OS se guarda el token y lo borra del archivo. "
          "Un tercer renglón opcional, favs=light.living,switch.cafetera, arma los favoritos."),
        aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
}

static void gear_cb(lv_event_t *ev)
{
    confirm_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = U.land ? 760 : U.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = box(U.overlay, w, U.H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 18, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *head = box(col, w, 110);
    lv_obj_t *b = box(head, LV_SIZE_CONTENT, 60);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, sheet_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = aos_label(b, "", aos_font_body, AOS_C_ACCENT);
    lv_label_set_text_fmt(bl, LV_SYMBOL_LEFT "  %s", aos_ha_location());
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(aos_label(head, _("Conexión"), aos_font_large, AOS_C_TEXT), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    config_card(col, w);
}

/* -------------------------------------------------------------------------- */
/* The page                                                                    */
/* -------------------------------------------------------------------------- */

static int area_used(int a)
{
    int n = 0;
    for (int i = 0; i < aos_ha_count(); i++) n += aos_ha_at(i)->area == a;
    return n;
}

static void tab_cb(lv_event_t *ev)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(ev);
    build();
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, int tab, bool vertical)
{
    bool on = S.tab == tab;
    lv_obj_t *c = box(parent, vertical ? lv_pct(100) : LV_SIZE_CONTENT, vertical ? 80 : 72);
    lv_obj_set_style_radius(c, vertical ? 22 : 36, 0);
    lv_obj_set_style_pad_hor(c, 26, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tab);
    lv_obj_t *l = aos_label(c, text, aos_font_body, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    if (vertical) lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    else lv_obj_center(l);
    if (!vertical) lv_obj_set_width(c, LV_SIZE_CONTENT);
    return c;
}

static void add_tabs(lv_obj_t *parent, bool vertical)
{
    chip(parent, _("Favoritos"), TAB_FAVS, vertical);
    for (int a = 0; a < aos_ha_area_count(); a++)
        if (area_used(a)) chip(parent, aos_ha_area_name(a), a, vertical);
    if (area_used(-1)) chip(parent, _("Otros"), TAB_OTHER, vertical);
}

static int by_kind(const void *x, const void *y)
{
    const aos_ha_entity_t *a = *(aos_ha_entity_t *const *)x, *b = *(aos_ha_entity_t *const *)y;
    if (a->domain != b->domain) return a->domain - b->domain;
    return strcmp(a->name, b->name);
}

/* The entities of the current tab, controls first; returns how many. */
static int collect(const aos_ha_entity_t **out, int max)
{
    int n = 0;
    if (S.tab == TAB_FAVS) {
        char ids[MAX_TILES][64];
        int nf = aos_ha_favs(ids, MAX_TILES);
        /* in the order they were starred, controls before sensors */
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < nf && n < max; k++) {
                int i = aos_ha_find(ids[k]);
                if (i >= 0 && is_sensor(aos_ha_at(i)) == pass) out[n++] = aos_ha_at(i);
            }
        return n;
    }
    for (int i = 0; i < aos_ha_count() && n < max; i++)
        if (aos_ha_at(i)->area == S.tab) out[n++] = aos_ha_at(i);
    qsort(out, n, sizeof *out, by_kind);
    return n;
}

static void section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *t = aos_label(parent, title, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_pad_left(t, 8, 0);
    lv_obj_set_style_pad_top(t, 8, 0);
}

static void empty_card(lv_obj_t *parent, int32_t w, const char *glyph, const char *title, const char *text)
{
    lv_obj_t *c = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 32, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 14, 0);
    aos_label(c, glyph, &aos_sym_72, C_HA);
    aos_label(c, title, aos_font_title, AOS_C_TEXT);
    lv_obj_t *l = aos_label(c, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, w - 64);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void fill_content(lv_obj_t *col, int32_t w)
{
    aos_ha_state_t st = aos_ha_state();
    if (st != AOS_HA_READY && !aos_ha_count()) {
        if (st == AOS_HA_UNCONFIGURED || st == AOS_HA_AUTH_FAILED)
            empty_card(col, w, AOS_SYM_HOME_ASSISTANT,
                       st == AOS_HA_AUTH_FAILED ? _("El token no sirve") : _("Conectá tu Home Assistant"),
                       st == AOS_HA_AUTH_FAILED ? _("Home Assistant lo rechazó. Generá uno nuevo y ponelo en ha.txt.")
                                                : _("Hace falta la dirección y un token de acceso."));
        else
            empty_card(col, w, AOS_SYM_HOME_ASSISTANT, state_name(st),
                       aos_ha_error()[0] && st == AOS_HA_ERROR ? aos_ha_error() : aos_ha_url());
        if (st == AOS_HA_UNCONFIGURED || st == AOS_HA_AUTH_FAILED || st == AOS_HA_ERROR) config_card(col, w);
        return;
    }
    const aos_ha_entity_t *list[MAX_TILES];
    int n = collect(list, MAX_TILES);
    if (!n) {
        empty_card(col, w, AOS_SYM_STAR_OUTLINE, _("Sin favoritos todavía"),
                   _("Mantené apretado un mosaico en cualquier ambiente y tocá la estrella."));
        return;
    }
    int cols = w > 1000 ? 4 : w > 700 ? 3 : 2;
    int32_t gap = 16, tw = (w - (cols - 1) * gap) / cols;
    lv_obj_t *grid = NULL;
    bool in_sensors = false;
    for (int k = 0; k < n && U.ntiles < MAX_TILES; k++) {
        bool sens = is_sensor(list[k]);
        if (!grid || (sens && !in_sensors)) {
            if (grid || S.tab != TAB_FAVS) section(col, sens ? _("SENSORES") : _("CONTROLES"));
            grid = box(col, w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_gap(grid, gap, 0);
            in_sensors = sens;
        }
        tile_create(grid, &U.tiles[U.ntiles++], list[k], tw);
    }
}

static lv_obj_t *column(lv_obj_t *parent, int32_t x, int32_t w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, U.H);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_style_pad_hor(c, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(c, 30, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static void head(lv_obj_t *col, int32_t w)
{
    lv_obj_t *h = box(col, w, 96);
    lv_obj_t *t = aos_label(h, aos_ha_location(), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_t *g = box(h, 80, 80);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g, gear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(aos_label(g, AOS_SYM_COG, &aos_sym_44, AOS_C_DIM));
    lv_obj_align(g, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    aos_ha_state_t st = aos_ha_state();
    if (st != AOS_HA_READY) {
        lv_obj_t *p = aos_label(h, state_name(st), aos_font_caption, st == AOS_HA_ERROR ? AOS_C_ORANGE : AOS_C_DIM);
        lv_obj_align(p, LV_ALIGN_TOP_LEFT, 6, 0);
    }
}

/* What the page is built from: when it changes the page is rebuilt,
 * otherwise the tiles are only refreshed. */
static uint32_t signature(void)
{
    uint32_t h = 2166136261u;
    #define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIX(aos_ha_state() == AOS_HA_READY);
    MIX(aos_ha_count());
    MIX(aos_ha_area_count());
    MIX(S.tab);
    char ids[MAX_TILES][64];
    int nf = aos_ha_favs(ids, MAX_TILES);
    for (int k = 0; k < nf; k++)
        for (const char *p = ids[k]; *p; p++) MIX(*p);
    return h;
}

static void build(void)
{
    if (U.page) lv_obj_delete(U.page);
    U.ntiles = 0;
    aos_ha_lock();
    U.sig = signature();
    if (S.tab >= aos_ha_area_count() && aos_ha_state() == AOS_HA_READY) S.tab = TAB_FAVS;
    U.page = box(U.root, U.W, U.H);
    if (!U.land) {
        lv_obj_t *col = column(U.page, 0, U.W);
        int32_t w = U.W - 2 * AOS_UI_PAD;
        head(col, w);
        if (aos_ha_count()) {
            lv_obj_t *tabs = box(col, w, 80);
            lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(tabs, 12, 0);
            lv_obj_add_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(tabs, LV_DIR_HOR);
            lv_obj_set_scrollbar_mode(tabs, LV_SCROLLBAR_MODE_OFF);
            add_tabs(tabs, false);
        }
        fill_content(col, w);
    } else {
        lv_obj_t *side = column(U.page, 0, SIDEBAR_W);
        lv_obj_set_style_pad_right(side, AOS_UI_PAD / 2, 0);
        lv_obj_set_style_pad_row(side, 8, 0);
        head(side, SIDEBAR_W - AOS_UI_PAD * 3 / 2);
        if (aos_ha_count()) add_tabs(side, true);
        lv_obj_t *main = column(U.page, SIDEBAR_W, U.W - SIDEBAR_W);
        lv_obj_set_style_pad_left(main, AOS_UI_PAD / 2, 0);
        lv_obj_set_style_pad_top(main, 24, 0);
        fill_content(main, U.W - SIDEBAR_W - AOS_UI_PAD * 3 / 2);
    }
    aos_ha_unlock();
    lv_obj_move_to_index(U.page, 0);            /* a sheet stays on top */
}

static void timer_cb(lv_timer_t *t)
{
    uint32_t v = aos_ha_version();
    /* climate: send the target once the +/- taps have stopped for a moment */
    if (U.sh_target_ms && (uint32_t)aos_hal_uptime_ms() - U.sh_target_ms > 700) {
        char d[48];
        snprintf(d, sizeof d, "{\"temperature\":%.1f}", U.sh_target);
        aos_ha_call("climate", "set_temperature", S.sheet, d);
        U.sh_target_ms = 0;
    }
    if (v == U.seen) return;
    U.seen = v;
    aos_ha_lock();
    uint32_t sig = signature();
    aos_ha_unlock();
    if (sig != U.sig && !U.kb) { build(); sheet_refresh(); return; }
    aos_ha_lock();
    for (int k = 0; k < U.ntiles; k++) {
        int i = aos_ha_find(U.tiles[k].id);
        if (i >= 0) tile_update(&U.tiles[k], aos_ha_at(i));
    }
    aos_ha_unlock();
    sheet_refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    aos_ha_start();
    char sheet[64];
    snprintf(sheet, sizeof sheet, "%s", S.sheet);
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.seen = aos_ha_version();
    build();
    if (sheet[0]) sheet_open(sheet);         /* a turn of the screen keeps the sheet */
    U.timer = lv_timer_create(timer_cb, 150, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    U.page = U.overlay = NULL;
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { confirm_close(); return true; }
    return false;
}

void aos_app_ha_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.ha", .name = "Home Assistant", .icon = AOS_SYM_HOME_ASSISTANT,
            .color_a = 0x41BDF5, .color_b = 0x0B6FB0,
            .flags = AOS_APP_FLAG_KEEP, .order = 100,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}

/* -------------------------------------------------------------------------- */
/* The home-screen widget                                                      */
/* -------------------------------------------------------------------------- */

#define W_TILES 4

typedef struct {
    lv_obj_t *title, *hint;
    lv_obj_t *tile[W_TILES], *glyph[W_TILES], *name[W_TILES], *state[W_TILES];
    char id[W_TILES][64];
    int n;
    uint32_t seen;
    lv_timer_t *timer;
} widget_t;

static void w_tile_cb(lv_event_t *ev)
{
    const char *id = lv_event_get_user_data(ev);
    if (!aos_ha_tap(id)) aos_ui_open("aos.ha");
}

static void w_open_cb(lv_event_t *ev) { aos_ui_open("aos.ha"); }

static void w_refresh(widget_t *w)
{
    aos_ha_lock();
    lv_label_set_text(w->title, aos_ha_location());
    char ids[W_TILES][64];
    int nf = aos_ha_favs(ids, W_TILES);
    bool ready = aos_ha_state() == AOS_HA_READY || aos_ha_count();
    int shown = 0;
    for (int k = 0; k < W_TILES; k++) {
        int i = k < nf && ready ? aos_ha_find(ids[k]) : -1;
        if (i < 0) { lv_obj_add_flag(w->tile[k], LV_OBJ_FLAG_HIDDEN); continue; }
        const aos_ha_entity_t *e = aos_ha_at(i);
        shown++;
        snprintf(w->id[k], sizeof w->id[k], "%s", e->id);
        lv_obj_remove_flag(w->tile[k], LV_OBJ_FLAG_HIDDEN);
        bool on = is_on(e);
        char st[48];
        if (is_sensor(e) && e->domain == HA_SENSOR) fmt_value(e, st, sizeof st);
        else state_text(e, st, sizeof st);
        lv_label_set_text(w->glyph[k], glyph_of(e));
        lv_obj_set_style_text_color(w->glyph[k], on || is_sensor(e) ? lv_color_hex(tint_of(e)) : lv_color_hex(0xAEAEB2), 0);
        lv_label_set_text(w->name[k], e->name);
        lv_label_set_text(w->state[k], st);
        lv_obj_set_style_bg_opa(w->tile[k], on ? LV_OPA_80 : LV_OPA_20, 0);
        lv_obj_set_style_text_color(w->name[k], on ? lv_color_hex(0x1C1C1E) : lv_color_white(), 0);
        lv_obj_set_style_text_color(w->state[k], on ? lv_color_hex(0x48484A) : lv_color_hex(0xC7C7CC), 0);
    }
    aos_ha_state_t st = aos_ha_state();
    lv_label_set_text(w->hint, shown ? "" : st == AOS_HA_UNCONFIGURED ? _("Tocá para configurar la conexión")
                             : !ready ? state_name(st) : _("Marcá favoritos en la app con la estrella"));
    aos_ha_unlock();
}

static void w_timer_cb(lv_timer_t *t)
{
    widget_t *w = lv_timer_get_user_data(t);
    uint32_t v = aos_ha_version();
    if (v != w->seen) { w->seen = v; w_refresh(w); }
}

static void w_deleted_cb(lv_event_t *ev)
{
    widget_t *w = lv_event_get_user_data(ev);
    lv_timer_delete(w->timer);
    free(w);
}

static void widget_create(lv_obj_t *card, int32_t cw, int32_t ch, const char *arg)
{
    (void)arg;
    aos_ha_start();
    widget_t *w = calloc(1, sizeof *w);
    if (!w) return;
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, w_open_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *g = aos_label(card, AOS_SYM_HOME_ASSISTANT, &aos_sym_28, C_HA);
    lv_obj_align(g, LV_ALIGN_TOP_LEFT, 0, 0);
    w->title = aos_label(card, "", aos_font_small, lv_color_white());
    lv_obj_align(w->title, LV_ALIGN_TOP_LEFT, 40, 0);
    w->hint = aos_label(card, "", aos_font_small, lv_color_hex(0xDDE6F0));
    lv_obj_set_width(w->hint, cw - 44);
    lv_label_set_long_mode(w->hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(w->hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    int cols = cw > 900 ? 4 : cw > 500 ? 4 : 2;
    int rows = W_TILES / cols;
    int32_t iw = cw - 44, ih = ch - 44 - 48, gap = 12;
    int32_t tw = (iw - (cols - 1) * gap) / cols, th = (ih - (rows - 1) * gap) / rows;
    for (int k = 0; k < W_TILES; k++) {
        lv_obj_t *t = box(card, tw, th);
        lv_obj_set_pos(t, (k % cols) * (tw + gap), 48 + (k / cols) * (th + gap));
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_bg_color(t, lv_color_white(), 0);
        lv_obj_set_style_pad_all(t, 14, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(t, w_tile_cb, LV_EVENT_CLICKED, w->id[k]);
        w->glyph[k] = aos_label(t, "", &aos_sym_44, lv_color_white());
        lv_obj_align(w->glyph[k], LV_ALIGN_TOP_LEFT, 0, 0);
        w->name[k] = aos_label(t, "", aos_font_caption, lv_color_white());
        lv_obj_set_size(w->name[k], tw - 28, lv_font_get_line_height(aos_font_caption));
        lv_label_set_long_mode(w->name[k], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(w->name[k], LV_ALIGN_BOTTOM_LEFT, 0, -24);
        w->state[k] = aos_label(t, "", aos_font_tiny, lv_color_white());
        lv_obj_set_size(w->state[k], tw - 28, lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(w->state[k], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(w->state[k], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        aos_make_decorative(w->glyph[k]);
        w->tile[k] = t;
    }
    w->seen = aos_ha_version();
    w_refresh(w);
    w->timer = lv_timer_create(w_timer_cb, 400, w);
    lv_obj_add_event_cb(card, w_deleted_cb, LV_EVENT_DELETE, w);
}

/* -------------------------------------------------------------------------- */
/* The Control Centre's tile: scenes and scripts a tap away                    */
/* -------------------------------------------------------------------------- */

#define CC_MAX 4

typedef struct {
    lv_obj_t *tile;
    char id[CC_MAX][64];
    int n;
    uint32_t seen;
    lv_timer_t *timer;
} cc_t;

static void cc_tap_cb(lv_event_t *ev)
{
    const char *id = lv_event_get_user_data(ev);
    if (aos_ha_tap(id)) {
        lv_obj_t *b = lv_event_get_current_target(ev);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xBF5AF2), 0);    /* it went */
    } else {
        aos_ui_toast(_("Home Assistant no está conectado"), 1500);
    }
}

/* Favourite scenes and scripts first, in the favourites' order, then the
 * house's other scenes. */
static int cc_pick(char ids[CC_MAX][64])
{
    int n = 0;
    char favs[64][64];
    int nf = aos_ha_favs(favs, 64);
    for (int k = 0; k < nf && n < CC_MAX; k++) {
        int i = aos_ha_find(favs[k]);
        if (i >= 0 && (aos_ha_at(i)->domain == HA_SCENE || aos_ha_at(i)->domain == HA_SCRIPT))
            snprintf(ids[n++], 64, "%.63s", favs[k]);
    }
    for (int i = 0; i < aos_ha_count() && n < CC_MAX; i++) {
        const aos_ha_entity_t *e = aos_ha_at(i);
        if (e->domain != HA_SCENE) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= !strcmp(ids[k], e->id);
        if (!dup) snprintf(ids[n++], 64, "%s", e->id);
    }
    return n;
}

static void cc_fill(cc_t *c)
{
    lv_obj_clean(c->tile);
    aos_ha_lock();
    c->n = aos_ha_state() == AOS_HA_READY || aos_ha_count() ? cc_pick(c->id) : 0;
    if (!c->n) {
        aos_ha_state_t st = aos_ha_state();
        lv_obj_t *g = aos_label(c->tile, AOS_SYM_HOME_ASSISTANT, &aos_sym_44, C_HA);
        lv_obj_t *l = aos_label(c->tile, st == AOS_HA_READY ? _("Sin escenas en Home Assistant") : state_name(st),
                                aos_font_caption, lv_color_hex(0x8A93A3));
        aos_make_decorative(g);
        aos_make_decorative(l);
    }
    for (int k = 0; k < c->n; k++) {
        int i = aos_ha_find(c->id[k]);
        const aos_ha_entity_t *e = aos_ha_at(i);
        lv_obj_t *b = box(c->tile, LV_SIZE_CONTENT, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 26, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A404B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x55606E), LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(b, 10, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 4, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cc_tap_cb, LV_EVENT_CLICKED, c->id[k]);
        lv_obj_t *g = aos_label(b, glyph_of(e), &aos_sym_28, lv_color_hex(0xE0B3FF));
        lv_obj_t *n = aos_label(b, e->name, aos_font_tiny, lv_color_white());
        lv_obj_set_width(n, lv_pct(100));
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        aos_make_decorative(g);
        aos_make_decorative(n);
    }
    aos_ha_unlock();
}

static void cc_timer_cb(lv_timer_t *t)
{
    cc_t *c = lv_timer_get_user_data(t);
    uint32_t v = aos_ha_version();
    if (v == c->seen) return;
    c->seen = v;
    /* rebuilt only when what it shows could change: the state, not a sensor */
    char ids[CC_MAX][64];
    aos_ha_lock();
    int n = aos_ha_state() == AOS_HA_READY || aos_ha_count() ? cc_pick(ids) : 0;
    bool same = n == c->n;
    for (int k = 0; same && k < n; k++) same = !strcmp(ids[k], c->id[k]);
    aos_ha_unlock();
    if (!same || !n) cc_fill(c);
}

static void cc_deleted_cb(lv_event_t *ev)
{
    cc_t *c = lv_event_get_user_data(ev);
    lv_timer_delete(c->timer);
    free(c);
}

static void cc_create(lv_obj_t *tile, int32_t w, int32_t h, const char *arg)
{
    (void)w; (void)h; (void)arg;
    aos_ha_start();
    cc_t *c = calloc(1, sizeof *c);
    if (!c) return;
    c->tile = tile;
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_style_pad_column(tile, 10, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->seen = aos_ha_version();
    cc_fill(c);
    c->timer = lv_timer_create(cc_timer_cb, 500, c);
    lv_obj_add_event_cb(tile, cc_deleted_cb, LV_EVENT_DELETE, c);
}

void aos_ha_widget_register(void)
{
    aos_ui_register_widget("ha", widget_create);
    aos_ui_register_widget("cc.ha", cc_create);
}
