/*
 * MAPAS - the panels over the map: the menu and the search.
 *
 * Both are ordinary LVGL screens that cover the map while they are up (the
 * frames stop, and the map's canvas is hidden under them). They are rebuilt
 * from scratch when the screen turns, with what was typed kept.
 *
 *   menu    save this view, the zones of zones.txt, Clima's city, the
 *           offline zones on the card (one row per zone, however many packs
 *           it came in), and the settings: downloading on or off, clearing
 *           the cache.
 *   search  the system's keyboard and the results under the text as it is
 *           typed: the offline indexes answer in a fraction of a second, so
 *           every pause in the typing searches them. The OK key also asks
 *           Photon online (never while typing: it is someone else's
 *           service). Upright: the text, the results, the keyboard below;
 *           turned: the text and the keyboard on the left, the results in a
 *           column on the right.
 */
#include "mapas.h"

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
#define TYPE_MS     300                 /* a pause this long in the typing searches */

/* ---------------------------------------------------------------------------
 * Common
 * ------------------------------------------------------------------------- */

void mp_show_map(app_t *a)
{
    if (a->panel) {
        lv_obj_delete(a->panel);
        a->panel = NULL;
    }
    a->ta = a->kb = a->res_list = a->res_note = NULL;
    a->viewing = true;
    a->screen = SCR_MAP;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    lv_obj_remove_flag(a->map, LV_OBJ_FLAG_HIDDEN);
    a->overlay_dirty = true;
}

static void back_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->search_cancel = true;
    mp_show_map(a);
}

/* A full-screen panel over the map, in place of whatever panel was up. The
 * left-edge swipe goes back again while it is up. */
static lv_obj_t *panel_new(app_t *a, int screen)
{
    if (a->panel) lv_obj_delete(a->panel);
    a->ta = a->kb = a->res_list = a->res_note = NULL;
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

/* A card row: an icon, a line of text, and a second, dimmer one. */
static lv_obj_t *row(lv_obj_t *parent, int w, const char *sym, const char *text, const char *sub,
                     lv_event_cb_t cb, void *user, int idx)
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
    }
    lv_obj_t *l = aos_label(r, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - tx - 20);
    lv_obj_align(l, sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, tx, sub ? 14 : 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (sub) {
        lv_obj_t *s = aos_label(r, sub, aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, w - tx - 20);
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

/* ---------------------------------------------------------------------------
 * The menu: zones, offline maps, settings
 * ------------------------------------------------------------------------- */

static void zone_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i >= 0 && i < a->nzones) mp_go_to(a, a->zones[i].cx, a->zones[i].cy, a->zones[i].z);
    mp_show_map(a);
}

static void pack_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i >= 0 && i < a->npacks) {
        const mp_pack_info_t *p = &a->packs[i];
        uint32_t x0, y0, x1, y1;
        mp_lonlat_to_world((float)p->w / 1e6f, (float)p->n / 1e6f, &x0, &y0);
        mp_lonlat_to_world((float)p->e / 1e6f, (float)p->s / 1e6f, &x1, &y1);
        /* the zoom that fits the zone on the screen, within what the pack has */
        float fw = (float)(x1 - x0) / (float)a->sw, fh = (float)(y1 - y0) / (float)a->sh;
        float span = fw > fh ? fw : fh;         /* world units per screen pixel */
        float z = span > 0 ? log2f(4294967296.0f / 256.0f / span) : 14.0f;
        if (z < p->minz + 1) z = (float)(p->minz + 1);
        if (z > p->maxz + 3) z = (float)(p->maxz + 3);
        mp_go_to(a, x0 + (x1 - x0) / 2, y0 + (y1 - y0) / 2, z);
    }
    mp_show_map(a);
}

static void clima_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int32_t la, lo;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        uint32_t x, y;
        mp_lonlat_to_world((float)lo / 1e4f, (float)la / 1e4f, &x, &y);
        mp_go_to(a, x, y, 14.0f);
    }
    mp_show_map(a);
}

static void save_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    char name[32];
    snprintf(name, sizeof name, "%s %d", _("Zona"), a->nzones + 1);
    if (mp_zone_append(a, name)) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: %s", _("Guardada"), name);
        aos_ui_toast(msg, 2000);
    } else {
        aos_ui_toast(_("No se pudo guardar"), 2000);
    }
    mp_show_map(a);
}

static void online_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->online = !a->online;
    aos_hal_pref_set_i32("map_online", a->online);
    lv_obj_t *r = lv_event_get_current_target(e);
    lv_obj_t *l = lv_obj_get_child(r, 1);
    if (l) lv_label_set_text(l, a->online ? _("Descargar: sí") : _("Descargar: no"));
    a->tgen++;
}

static void clear_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->cleared = -1;
    a->want_clear = true;
    aos_ui_toast(_("Caché borrada"), 1500);
}

static void search_open_cb(lv_event_t *e)
{
    mp_show_search((app_t *)lv_event_get_user_data(e));
}

void mp_show_list(app_t *a)
{
    mp_zones_load(a);
    a->want_rescan = true;

    lv_obj_t *p = panel_new(a, SCR_LIST);
    int w = a->sw - 2 * PAD;
    if (w > 760) w = 760;

    lv_obj_t *back = round_btn(p, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, PAD, PAD);
    lv_obj_t *title = aos_label(p, _("Mapas"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 104, PAD + 18);

    lv_obj_t *l = lv_obj_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, a->sw, a->sh - (PAD + 100));
    lv_obj_set_pos(l, 0, PAD + 100);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 12, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_obj_set_style_pad_bottom(l, 80, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);

    row(l, w, AOS_SYM_MAGNIFY, _("Buscar"), NULL, search_open_cb, a, 0);
    row(l, w, AOS_SYM_STAR_OUTLINE, _("Guardar esta vista"), NULL, save_cb, a, 0);

    if (a->nzones) heading(l, w, _("Zonas"));
    for (int i = 0; i < a->nzones; i++) {
        char sub[32];
        snprintf(sub, sizeof sub, "zoom %.1f", (double)a->zones[i].z);
        row(l, w, AOS_SYM_FLAG_OUTLINE, a->zones[i].name, sub, zone_cb, a, i);
    }
    int32_t la, lo;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        char city[48] = "";
        aos_hal_pref_get_str("clima_city", city, sizeof city);
        row(l, w, AOS_SYM_WEATHER_PARTLY_CLOUDY, _("Ciudad de Clima"), city[0] ? city : NULL, clima_cb, a, 0);
    }

    if (a->packs_ready && a->npacks) {
        heading(l, w, _("Sin conexión"));
        /* a zone bigger than one upload is several packs with the same name:
         * one row, the tiles added up */
        for (int i = 0; i < a->npacks; i++) {
            bool seen = false;
            for (int j = 0; j < i; j++)
                if (!strcmp(a->packs[j].name, a->packs[i].name)) seen = true;
            if (seen) continue;
            int n = 0, zmin = 99, zmax = 0;
            for (int j = i; j < a->npacks; j++) {
                if (strcmp(a->packs[j].name, a->packs[i].name)) continue;
                n += a->packs[j].ntiles;
                if (a->packs[j].minz < zmin) zmin = a->packs[j].minz;
                if (a->packs[j].maxz > zmax) zmax = a->packs[j].maxz;
            }
            char sub[48];
            snprintf(sub, sizeof sub, "%d %s · z%d-%d", n, _("teselas"), zmin, zmax);
            row(l, w, AOS_SYM_SD, a->packs[i].name, sub, pack_cb, a, i);
        }
    }

    heading(l, w, _("Ajustes"));
    row(l, w, AOS_SYM_DOWNLOAD, a->online ? _("Descargar: sí") : _("Descargar: no"), NULL, online_cb, a, 0);
    row(l, w, AOS_SYM_DELETE, _("Borrar caché"), NULL, clear_cb, a, 0);

    char info[160];
    snprintf(info, sizeof info, "%s\n%s %u KB",
             _("Datos: OpenFreeMap, OpenMapTiles, OpenStreetMap"),
             _("Descargado:"), (unsigned)(a->downloaded / 1024));
    lv_obj_t *t = aos_label(l, info, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(t, 18, 0);
}

/* ---------------------------------------------------------------------------
 * Search
 * ------------------------------------------------------------------------- */

/* What a point of interest is: the index and Photon carry OpenStreetMap's
 * class ("railway", "fast_food") and three kinds of the index's own
 * ("parque", "aeropuerto", "cerro"), none of them for the screen. The
 * commonest, in the order they come in a city's index, in the board's
 * language; any other, as it came, with its underscores out. */
static const struct {
    const char *k, *es;
} KIND_NAMES[] = {
    { "parque", N_("parque") },
    { "aeropuerto", N_("aeropuerto") },
    { "cerro", N_("cerro") },
    { "shop", N_("comercio") },
    { "school", N_("escuela") },
    { "restaurant", N_("restaurante") },
    { "office", N_("oficina") },
    { "car", N_("autos") },
    { "clothing_store", N_("ropa") },
    { "grocery", N_("almacén") },
    { "park", N_("parque") },
    { "cafe", N_("café") },
    { "hospital", N_("hospital") },
    { "fast_food", N_("comida rápida") },
    { "pharmacy", N_("farmacia") },
    { "butcher", N_("carnicería") },
    { "hairdresser", N_("peluquería") },
    { "bakery", N_("panadería") },
    { "place_of_worship", N_("templo") },
    { "sports_centre", N_("club deportivo") },
    { "bar", N_("bar") },
    { "lodging", N_("alojamiento") },
    { "college", N_("universidad") },
    { "town_hall", N_("edificio público") },
    { "ice_cream", N_("heladería") },
    { "art_gallery", N_("galería de arte") },
    { "library", N_("biblioteca") },
    { "police", N_("policía") },
    { "bicycle_rental", N_("bicis de alquiler") },
    { "veterinary", N_("veterinaria") },
    { "railway", N_("estación") },
    { "pitch", N_("cancha") },
    { "parking", N_("estacionamiento") },
    { "doctors", N_("consultorio") },
    { "bicycle", N_("bicicletería") },
    { "beer", N_("cervecería") },
    { "museum", N_("museo") },
    { "laundry", N_("lavandería") },
    { "alcohol_shop", N_("vinoteca") },
    { "theatre", N_("teatro") },
    { "dentist", N_("dentista") },
    { "attraction", N_("atracción") },
    { "bank", N_("banco") },
    { "fuel", N_("estación de servicio") },
    { "stadium", N_("estadio") },
    { "fire_station", N_("bomberos") },
    { "music", N_("música") },
    { "atm", N_("cajero") },
    { "monument", N_("monumento") },
    { "cinema", N_("cine") },
    { "cemetery", N_("cementerio") },
    { "playground", N_("juegos infantiles") },
    { "information", N_("información") },
    { "post", N_("correo") },
    { "swimming_pool", N_("pileta") },
    { "garden", N_("jardín") },
    { "harbor", N_("puerto") },
    { "theme_park", N_("parque temático") },
    { "ferry_terminal", N_("terminal de ferry") },
    { "prison", N_("cárcel") },
    { "campsite", N_("camping") },
    { "castle", N_("castillo") },
    { "toilets", N_("baños") },
    { "zoo", N_("zoológico") },
    { "aquarium", N_("acuario") },
};

static const char *kind_text(const mp_hit_t *h)
{
    switch (h->kind) {
    case MP_KIND_PLACE:   return _("lugar");
    case MP_KIND_STREET:  return _("calle");
    case MP_KIND_WATER:   return _("agua");
    case MP_KIND_ADDRESS: return h->sub;    /* the town, from Photon */
    default:              break;
    }
    for (size_t i = 0; i < sizeof KIND_NAMES / sizeof KIND_NAMES[0]; i++)
        if (!strcmp(h->sub, KIND_NAMES[i].k)) return _(KIND_NAMES[i].es);
    static char raw[40];
    snprintf(raw, sizeof raw, "%s", h->sub);
    for (char *c = raw; *c; c++)
        if (*c == '_') *c = ' ';
    return raw;
}

static void result_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->nhits) return;
    const mp_hit_t *h = &a->hits[i];
    uint32_t x, y;
    mp_lonlat_to_world((float)h->lon / 1e6f, (float)h->lat / 1e6f, &x, &y);
    a->pin_on = true;
    a->pin_cx = x;
    a->pin_cy = y;
    snprintf(a->pin_name, sizeof a->pin_name, "%s", h->name);
    mp_go_to(a, x, y, mp_search_zoom(h));
    mp_show_map(a);
}

static void results_refresh(app_t *a)
{
    if (!a->res_list) return;
    lv_obj_clean(a->res_list);
    lv_obj_t *l = a->res_list;
    lv_obj_update_layout(l);            /* its size may have just been set */
    int w = lv_obj_get_width(l) - 8;
    const char *note = NULL;
    if (!a->query[0]) {
        note = _("Escribí un lugar, una calle o un comercio.");
    } else if (a->searching || a->want_search || a->typed || (a->photon_id > 0 && !a->nhits)) {
        note = _("Buscando...");
    } else if (!a->nhits) {
        note = a->online ? _("Nada con ese nombre.") : _("Nada con ese nombre en las zonas descargadas.");
    }
    for (int i = 0; i < a->nhits; i++) {
        const mp_hit_t *h = &a->hits[i];
        row(l, w, h->score >= 1000 ? AOS_SYM_WIFI : NULL, h->name, kind_text(h), result_cb, a, i);
    }
    if (!note && a->photon_id > 0) note = _("Buscando en línea...");
    if (note) {
        lv_obj_t *t = aos_label(l, note, aos_font_body, AOS_C_DIM);
        lv_obj_set_width(t, w);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(t, 20, 0);
    }
}

/* the worker's offline pass for what is typed now */
static void search_offline(app_t *a)
{
    a->typed = false;
    mp_search_key(a->query, a->skey, sizeof a->skey);
    if (!a->skey[0]) {
        a->nhits = 0;
        results_refresh(a);
        return;
    }
    a->search_cancel = true;            /* a pass under way for older text stops */
    a->want_search = true;
    results_refresh(a);
}

static void ta_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->ta) return;
    snprintf(a->query, sizeof a->query, "%s", lv_textarea_get_text(a->ta));
    a->typed = true;
    a->typed_ms = (uint32_t)aos_hal_uptime_ms();
    a->photon_asked = false;
    a->photon_done = false;
}

static void kb_show(app_t *a, bool on);

static void ta_click_cb(lv_event_t *e)
{
    kb_show((app_t *)lv_event_get_user_data(e), true);
}

static void kb_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        /* OK: search now, online too, and the keyboard goes to make room */
        if (a->ta) snprintf(a->query, sizeof a->query, "%s", lv_textarea_get_text(a->ta));
        search_offline(a);
        a->photon_asked = a->query[0] != 0;
        a->photon_done = false;
        kb_show(a, false);
    } else if (code == LV_EVENT_CANCEL) {
        kb_show(a, false);
    }
}

/* Where the text, the results and the keyboard go, for the keyboard up or
 * down. */
static void search_layout(app_t *a, bool kb_on)
{
    int sw = a->sw, sh = a->sh;
    bool land = sw > sh;
    int ty = PAD, th = 80, tx = PAD + 96;
    int col = land ? sw * 3 / 5 : sw;           /* the text's (and keyboard's) column */
    lv_obj_set_pos(a->ta, tx, ty);
    lv_obj_set_size(a->ta, (land && kb_on ? col : sw) - tx - PAD, th);
    int kh = land ? sh - (ty + th + 16) : sh * 2 / 5;
    if (a->kb) {
        lv_obj_set_size(a->kb, col, kh);
        /* the keyboard aligns itself to the bottom when created: a plain
         * position would count from there */
        lv_obj_align(a->kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_flag(a->kb, LV_OBJ_FLAG_HIDDEN, !kb_on);
    }
    int rx, ry, rw, rh;
    if (land && kb_on) {
        rx = col + 8;
        ry = PAD;
        rw = sw - col - 8 - PAD;
        rh = sh - PAD;
    } else {
        rx = PAD;
        ry = ty + th + 16;
        rw = sw - 2 * PAD;
        rh = sh - ry - (kb_on ? kh + 8 : 0);
    }
    lv_obj_set_pos(a->res_list, rx, ry);
    lv_obj_set_size(a->res_list, rw, rh);
}

static void kb_show(app_t *a, bool on)
{
    if (!a->ta || !a->kb) return;
    search_layout(a, on);
    results_refresh(a);
}

void mp_show_search(app_t *a)
{
    lv_obj_t *p = panel_new(a, SCR_SEARCH);
    lv_obj_t *back = round_btn(p, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, PAD, PAD);

    a->ta = lv_textarea_create(p);
    lv_textarea_set_one_line(a->ta, true);
    lv_textarea_set_max_length(a->ta, sizeof a->query - 8);
    lv_textarea_set_placeholder_text(a->ta, _("Buscar un lugar o una calle"));
    lv_obj_set_style_text_font(a->ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(a->ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(a->ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(a->ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(a->ta, 0, 0);
    lv_obj_set_style_radius(a->ta, 40, 0);
    lv_obj_set_style_pad_hor(a->ta, 28, 0);
    lv_obj_set_style_pad_ver(a->ta, 20, 0);
    lv_obj_add_event_cb(a->ta, ta_click_cb, LV_EVENT_CLICKED, a);

    a->res_list = lv_obj_create(p);
    lv_obj_remove_style_all(a->res_list);
    lv_obj_add_flag(a->res_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(a->res_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->res_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(a->res_list, 10, 0);
    lv_obj_set_style_pad_bottom(a->res_list, 24, 0);
    lv_obj_set_scroll_dir(a->res_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->res_list, LV_SCROLLBAR_MODE_OFF);

    a->kb = lv_keyboard_create(p);
    aos_keyboard_style(a->kb, aos_font_body);
    lv_keyboard_set_textarea(a->kb, a->ta);
    lv_obj_add_event_cb(a->kb, kb_cb, LV_EVENT_READY, a);
    lv_obj_add_event_cb(a->kb, kb_cb, LV_EVENT_CANCEL, a);

    /* what was typed before comes back (the screen turned, or a second
     * search), and then the changes are what count */
    if (a->query[0]) lv_textarea_set_text(a->ta, a->query);
    lv_obj_add_event_cb(a->ta, ta_cb, LV_EVENT_VALUE_CHANGED, a);
    search_layout(a, true);
    results_refresh(a);
}

void mp_ui_relayout(app_t *a)
{
    if (a->screen == SCR_LIST) {
        mp_show_list(a);
    } else if (a->screen == SCR_SEARCH) {
        bool kb_on = a->kb && !lv_obj_has_flag(a->kb, LV_OBJ_FLAG_HIDDEN);
        mp_show_search(a);
        if (!kb_on) kb_show(a, false);
    }
}

/* In the tick: a pause in the typing starts the offline pass; the pass's
 * answer is shown and, after OK, Photon is asked. */
void mp_search_pump(app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (a->screen == SCR_SEARCH && a->typed && now - a->typed_ms >= TYPE_MS) search_offline(a);

    /* the worker's hits are read only while it is not writing them: it
     * raises searching before it takes want_search down */
    if (!a->want_search && !a->searching && a->search_seq != a->search_shown) {
        a->search_shown = a->search_seq;
        int n = a->nwhits;
        memcpy(a->hits, a->whits, (size_t)n * sizeof(mp_hit_t));
        a->nhits = n;
        if (a->screen == SCR_SEARCH) results_refresh(a);
    }
    bool idle = !a->want_search && !a->searching && !a->typed;
    if (idle && a->photon_asked && !a->photon_done && a->photon_id == 0 && a->screen == SCR_SEARCH) {
        a->photon_asked = false;
        if (a->online && aos_hal_net_state() == AOS_NET_CONNECTED) {
            float lon, lat;
            mp_world_to_lonlat(a->view.cx, a->view.cy, &lon, &lat);
            char url[240], q[160];
            /* the query, URL-encoded (letters, digits and a few signs) */
            int o = 0;
            for (const char *c = a->query; *c && o < (int)sizeof q - 4; c++) {
                if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) q[o++] = *c;
                else o += snprintf(q + o, sizeof q - (size_t)o, "%%%02X", (unsigned char)*c);
            }
            q[o] = 0;
            snprintf(url, sizeof url, "https://photon.komoot.io/api/?limit=8&lat=%.4f&lon=%.4f&q=%s",
                     (double)lat, (double)lon, q);
            a->photon_id = aos_hal_http_get(url, 48 * 1024);
            if (a->photon_id <= 0) a->photon_id = 0;
            results_refresh(a);
        } else {
            a->photon_done = true;
        }
    }
    const char *pick = getenv("MAPAS_PICK");
    if (pick && pick[0] == '1' && a->screen == SCR_SEARCH && idle && a->photon_id == 0 && a->nhits &&
        a->res_list && a->search_seq) {
        lv_obj_t *r = lv_obj_get_child(a->res_list, 0);
        if (r) lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
        return;
    }
    if (a->photon_id > 0) {
        aos_http_state_t hs = aos_hal_http_state(a->photon_id);
        if (hs == AOS_HTTP_BUSY) return;
        if (hs == AOS_HTTP_DONE) {
            int n = a->nhits;
            n += mp_search_photon(aos_hal_http_body(a->photon_id), a->hits, n, MAX_HITS);
            a->nhits = n;
        } else {
            aos_hal_log("mapas", "photon: %d", aos_hal_http_status(a->photon_id));
        }
        aos_hal_http_release(a->photon_id);
        a->photon_id = 0;
        a->photon_done = true;
        if (a->screen == SCR_SEARCH) results_refresh(a);
    }
}
