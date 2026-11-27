/*
 * Malla - the nodes: the list, and one of them in full.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * The list is the node's database as it sent it at connect time, kept up
 * to date by every packet heard: ours first, then the most recently heard.
 * A node in full has what is known of it, buttons to write to it, trace
 * the route to it and ask it for its telemetry or position, and the
 * telemetry it sent over time as charts.
 *
 * A traceroute (TRACEROUTE_APP with want_response) comes back with the
 * nodes it went through each way and the SNR each hop was heard with, in
 * quarters of a dB; firmware older than 2.5 sends no way back.
 */
#include "msh_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_MAX 160

/* ---- the list ---- */

static void node_cb(lv_event_t *e)
{
    msh_ui_open_node((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static int cmp_nodes(const void *a, const void *b)
{
    const msh_node_t *x = *(const msh_node_t *const *)a, *y = *(const msh_node_t *const *)b;
    if (x->num == MSH.my_num) return -1;
    if (y->num == MSH.my_num) return 1;
    if (x->favorite != y->favorite) return x->favorite ? -1 : 1;
    int32_t ax = msh_node_age(x), ay = msh_node_age(y);
    if (ax < 0) ax = 0x7FFFFFFF;
    if (ay < 0) ay = 0x7FFFFFFF;
    return ax < ay ? -1 : ax > ay ? 1 : 0;
}

static void hw_text(int model, char *out, size_t cap)
{
    const char *h = msh_hw_name(model);
    msh_scpy(out, cap, h && model ? h : "");
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

/* "85 % · SNR 6,5 dB · 2 saltos" */
static void signal_line(const msh_node_t *n, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    char v[16];
    if (n->battery > 100) o += (size_t)snprintf(out + o, cap - o, "%s", _("enchufado"));
    else if (n->battery >= 0) o += (size_t)snprintf(out + o, cap - o, "%d %%", n->battery);
    if (n->num != MSH.my_num) {
        if (n->hops == 0 && (n->snr != 0 || n->has_rssi)) {
            msh_ui_num(v, sizeof v, n->snr, 1);
            o += (size_t)snprintf(out + o, cap - o, "%sSNR %s dB", o ? "  ·  " : "", v);
            if (n->has_rssi && o < cap) o += (size_t)snprintf(out + o, cap - o, "  %d dBm", n->rssi);
        }
        if (n->hops > 0 && o < cap) o += (size_t)snprintf(out + o, cap - o, n->hops == 1 ? _("%s1 salto") : _("%s%d saltos"), o ? "  ·  " : "", n->hops);
        if (n->hops == 0 && o < cap) o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "  ·  " : "", _("directo"));
        if (n->via_mqtt && o < cap) o += (size_t)snprintf(out + o, cap - o, "%sMQTT", o ? "  ·  " : "");
    }
}

static void node_row(lv_obj_t *col, const msh_node_t *n)
{
    lv_obj_t *card = msh_ui_card(col);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 18, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, node_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)n->num);
    msh_ui_avatar(card, n->num, 76);

    lv_obj_t *mid = lv_obj_create(card);
    lv_obj_remove_style_all(mid);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 4, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);

    char name[48], safe[120], line[160], hw[40];
    msh_node_name(n->num, name, sizeof name);
    snprintf(line, sizeof line, "%s%s", name, n->num == MSH.my_num ? "  ·  " : "");
    /* the symbol after the filter, which takes it for a character the font lacks */
    snprintf(safe, sizeof safe, "%s", n->favorite ? LV_SYMBOL_BELL " " : "");
    msh_ui_safe(safe + strlen(safe), sizeof safe - strlen(safe), line);
    if (n->num == MSH.my_num) {
        size_t l = strlen(safe);
        snprintf(safe + l, sizeof safe - l, "%s", _("este"));
    }
    lv_obj_t *nl = msh_ui_label(mid, safe, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);

    hw_text(n->hw_model, hw, sizeof hw);
    char sig[96];
    signal_line(n, sig, sizeof sig);
    snprintf(line, sizeof line, "%s%s%s", hw, hw[0] && sig[0] ? "  ·  " : "", sig);
    lv_obj_t *l2 = msh_ui_label(mid, line, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l2, LV_PCT(100));
    lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);

    char age[32], dist[24] = "";
    msh_ui_age(n->num == MSH.my_num ? 0 : msh_node_age(n), age, sizeof age);
    float d = msh_node_dist(n);
    if (d >= 0) msh_ui_dist(dist, sizeof dist, d);
    snprintf(line, sizeof line, "%s%s%s", n->num == MSH.my_num ? _("conectado") : age, dist[0] ? "  ·  " : "", dist);
    msh_ui_label(mid, line, aos_font_small, AOS_C_DIM);
}

void msh_ui_nodes_build(lv_obj_t *body)
{
    lv_obj_t *col = msh_ui_col(body);
    msh_lock();
    static msh_node_t *order[MSH_NODES_MAX];
    int n = MSH.n_nodes;
    for (int i = 0; i < n; i++) order[i] = &MSH.nodes[i];
    qsort(order, (size_t)n, sizeof order[0], cmp_nodes);
    char head[80];
    snprintf(head, sizeof head, n == 1 ? _("%d nodo") : _("%d nodos"), n);
    msh_ui_label(col, head, aos_font_small, AOS_C_DIM);
    for (int i = 0; i < n && i < LIST_MAX; i++) node_row(col, order[i]);
    msh_unlock();
    if (!n) {
        lv_obj_t *l = msh_ui_label(col, _("Los nodos aparecen acá cuando el tuyo manda su lista."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
}

/* ---- one node ---- */

static void dm_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_open_chat(UI.node_num, 0);
}

static void trace_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    if (msh_send_traceroute(UI.node_num)) aos_ui_toast(_("Buscando la ruta…"), 1500);
    UI.dirty = true;
}

static void tele_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    if (msh_send_telemetry_req(UI.node_num)) aos_ui_toast(_("Pedido: la telemetría llega cuando conteste"), 2000);
}

static void pos_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    if (msh_send_position_req(UI.node_num)) aos_ui_toast(_("Pedido: la posición llega cuando conteste"), 2000);
}

static void fav_cb(lv_event_t *e)
{
    (void)e;
    msh_lock();
    msh_node_t *n = msh_node_find(UI.node_num);
    bool fav = n && n->favorite;
    msh_unlock();
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    msh_set_favorite(UI.node_num, !fav);
    UI.dirty = true;
}

static void map_cb(lv_event_t *e)
{
    (void)e;
    msh_lock();
    msh_node_t *n = msh_node_find(UI.node_num);
    if (n && n->has_pos) {
        UI.map_lat = n->lat_i;
        UI.map_lon = n->lon_i;
        if (UI.map_mpp <= 0 || UI.map_mpp > 20) UI.map_mpp = 8;
        UI.map_fitted = true;
    }
    msh_unlock();
    msh_ui_show_tab(MSH_TAB_MAP);
}

static void kv(lv_obj_t *card, const char *k, const char *v)
{
    lv_obj_t *r = msh_ui_row(card);
    lv_obj_t *kl = msh_ui_label(r, k, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(kl, UI.land ? 260 : 230);
    lv_obj_t *vl = msh_ui_label(r, v, aos_font_small, AOS_C_TEXT);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_MODE_WRAP);
}

static void hop_name(uint32_t num, char *out, size_t cap)
{
    char sn[8];
    if (num == MSH.my_num) msh_scpy(out, cap, _("este"));
    else msh_scpy(out, cap, msh_node_short(num, sn, sizeof sn));
}

/* "este → A (6,5) → B (2,0) → Dest (-3,25)": the names already filtered,
 * so the arrows (symbols the filter would drop) survive */
static void route_text(uint32_t from, const uint32_t *via, int n, const int8_t *snr, uint32_t to, char *out, size_t cap)
{
    size_t o = 0;
    char raw[16], nm[48], v[16];
    hop_name(from, raw, sizeof raw);
    msh_ui_safe(nm, sizeof nm, raw);
    o += (size_t)snprintf(out, cap, "%s", nm);
    for (int i = 0; i <= n && o < cap; i++) {
        uint32_t h = i < n ? via[i] : to;
        hop_name(h, raw, sizeof raw);
        msh_ui_safe(nm, sizeof nm, raw);
        if (snr[i] != INT8_MIN) {
            msh_ui_num(v, sizeof v, snr[i] / 4.0f, 1);
            o += (size_t)snprintf(out + o, cap - o, "  " LV_SYMBOL_RIGHT "  %s (%s dB)", nm, v);
        } else {
            o += (size_t)snprintf(out + o, cap - o, "  " LV_SYMBOL_RIGHT "  %s", nm);
        }
    }
}

static void trace_card(lv_obj_t *col)
{
    msh_trace_t *t = &MSH.trace;
    if (t->target != UI.node_num || !t->state) return;
    lv_obj_t *c = msh_ui_card(col);
    msh_ui_label(c, _("Traceroute"), aos_font_body, AOS_C_TEXT);
    char a[400];
    if (t->state == 1) {
        msh_ui_label(c, _("Esperando la respuesta…"), aos_font_small, AOS_C_DIM);
        return;
    }
    if (t->state == 3) {
        msh_ui_label(c, _("No hubo respuesta"), aos_font_small, AOS_C_RED);
        return;
    }
    route_text(MSH.my_num, t->to, t->n_to, t->snr_to, t->target, a, sizeof a);
    kv(c, _("Ida"), a);
    if (t->n_back || t->snr_back[0] != INT8_MIN) {
        route_text(t->target, t->back, t->n_back, t->snr_back, MSH.my_num, a, sizeof a);
        kv(c, _("Vuelta"), a);
    }
}

typedef float (*tget_t)(const msh_tsample_t *s);
static float g_batt(const msh_tsample_t *s) { return s->batt > 100 ? 100 : s->batt; }
static float g_volt(const msh_tsample_t *s) { return s->volt; }
static float g_chu(const msh_tsample_t *s) { return s->chu; }
static float g_air(const msh_tsample_t *s) { return s->air; }
static float g_temp(const msh_tsample_t *s) { return s->temp; }
static float g_hum(const msh_tsample_t *s) { return s->hum; }
static float g_press(const msh_tsample_t *s) { return s->press; }

/* A chart of one or two series of the node's telemetry, with the range and
 * the last value written above it. Scaled by 10 so a voltage keeps its
 * decimal. */
static void chart(lv_obj_t *col, const msh_tele_t *h, const char *title, const char *unit, int dec,
                  uint8_t has1, tget_t g1, uint8_t has2, tget_t g2, float lo_min, float hi_min)
{
    int n1 = 0;
    float lo = 1e9f, hi = -1e9f, last = 0;
    for (int i = 0; i < h->n; i++) {
        const msh_tsample_t *s = &h->s[(h->head + i) % MSH_TELE_N];
        if (s->has & has1) {
            float v = g1(s);
            n1++;
            last = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (g2 && (s->has & has2)) {
            float v = g2(s);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (n1 < 2) return;
    if (lo > lo_min) lo = lo_min;
    if (hi < hi_min) hi = hi_min;
    if (hi - lo < 1) hi = lo + 1;
    lv_obj_t *c = msh_ui_card(col);
    char t[96], v[16], a[16], b[16];
    msh_ui_num(v, sizeof v, last, dec);
    msh_ui_num(a, sizeof a, lo, 0);
    msh_ui_num(b, sizeof b, hi, 0);
    snprintf(t, sizeof t, "%s: %s %s", title, v, unit);
    msh_ui_label(c, t, aos_font_small, AOS_C_TEXT);
    lv_obj_t *ch = lv_chart_create(c);
    lv_obj_set_size(ch, LV_PCT(100), 200);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, (uint32_t)n1);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, (int32_t)(lo * 10), (int32_t)(hi * 10 + 9));
    lv_chart_set_div_line_count(ch, 4, 0);
    lv_obj_set_style_bg_color(ch, AOS_C_BG, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_line_color(ch, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 4, LV_PART_ITEMS);
    lv_chart_series_t *s1 = lv_chart_add_series(ch, AOS_C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *s2 = g2 ? lv_chart_add_series(ch, AOS_C_ORANGE, LV_CHART_AXIS_PRIMARY_Y) : NULL;
    for (int i = 0; i < h->n; i++) {
        const msh_tsample_t *s = &h->s[(h->head + i) % MSH_TELE_N];
        if (!(s->has & has1)) continue;
        lv_chart_set_next_value(ch, s1, (int32_t)(g1(s) * 10));
        if (s2) lv_chart_set_next_value(ch, s2, (s->has & has2) ? (int32_t)(g2(s) * 10) : LV_CHART_POINT_NONE);
    }
    snprintf(t, sizeof t, "%s %s  ·  %s %s  ·  %d %s", _("mín."), a, _("máx."), b, n1, _("lecturas"));
    msh_ui_label(c, t, aos_font_caption, AOS_C_DIM);
}

void msh_ui_node_build(lv_obj_t *body)
{
    lv_obj_t *col = msh_ui_col(body);
    msh_lock();
    msh_node_t *n = msh_node_find(UI.node_num);
    if (!n) {
        msh_unlock();
        msh_ui_label(col, _("Este nodo ya no está en la lista."), aos_font_small, AOS_C_DIM);
        return;
    }
    bool me = n->num == MSH.my_num;
    char name[48], safe[160], line[200], v[32], v2[32];

    lv_obj_t *top = msh_ui_card(col);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 22, 0);
    msh_ui_avatar(top, n->num, 120);
    lv_obj_t *tc = lv_obj_create(top);
    lv_obj_remove_style_all(tc);
    lv_obj_set_flex_grow(tc, 1);
    lv_obj_set_height(tc, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tc, 6, 0);
    msh_node_name(n->num, name, sizeof name);
    msh_ui_safe(safe, sizeof safe, name);
    lv_obj_t *nl = msh_ui_label(tc, safe, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_WRAP);
    char id[12], hw[40];
    msh_node_id(n->num, id);
    hw_text(n->hw_model, hw, sizeof hw);
    snprintf(line, sizeof line, "%s  ·  %s", id, n->has_user ? msh_role_name(n->role) : _("sin datos todavía"));
    msh_ui_label(tc, line, aos_font_small, AOS_C_DIM);
    if (hw[0]) msh_ui_label(tc, hw, aos_font_small, AOS_C_DIM);

    lv_obj_t *btns = msh_ui_row(col);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(btns, 12, 0);
    if (!me) {
        msh_ui_btn(btns, _("Mensaje"), AOS_C_ACCENT, dm_cb, NULL);
        msh_ui_btn(btns, _("Traceroute"), AOS_C_CARD2, trace_cb, NULL);
    }
    msh_ui_btn(btns, _("Telemetría"), AOS_C_CARD2, tele_cb, NULL);
    if (!me) msh_ui_btn(btns, _("Posición"), AOS_C_CARD2, pos_cb, NULL);
    if (n->has_pos) msh_ui_btn(btns, _("En el mapa"), AOS_C_CARD2, map_cb, NULL);
    if (!me) msh_ui_btn(btns, n->favorite ? _("Quitar favorito") : _("Favorito"), AOS_C_CARD2, fav_cb, NULL);

    trace_card(col);

    lv_obj_t *c = msh_ui_card(col);
    if (!me) {
        msh_ui_age(msh_node_age(n), v, sizeof v);
        kv(c, _("Visto"), v);
        signal_line(n, line, sizeof line);
        if (n->hops == 0 && (n->snr != 0 || n->has_rssi)) {
            msh_ui_num(v, sizeof v, n->snr, 2);
            if (n->has_rssi) snprintf(line, sizeof line, "SNR %s dB  ·  RSSI %d dBm", v, n->rssi);
            else snprintf(line, sizeof line, "SNR %s dB", v);
            kv(c, _("Señal"), line);
        }
        if (n->hops > 0) {
            snprintf(line, sizeof line, "%d", n->hops);
            kv(c, _("Saltos"), line);
        } else if (n->hops == 0) {
            kv(c, _("Saltos"), _("directo"));
        }
    }
    if (n->battery >= 0) {
        if (n->battery > 100) snprintf(line, sizeof line, "%s", _("enchufado"));
        else snprintf(line, sizeof line, "%d %%", n->battery);
        if (n->voltage > 0) {
            msh_ui_num(v, sizeof v, n->voltage, 2);
            size_t l = strlen(line);
            snprintf(line + l, sizeof line - l, "  ·  %s V", v);
        }
        kv(c, _("Batería"), line);
    }
    if (n->has_dev) {
        msh_ui_num(v, sizeof v, n->ch_util, 1);
        msh_ui_num(v2, sizeof v2, n->air_tx, 1);
        snprintf(line, sizeof line, "%s %%  ·  %s %s %%", v, _("aire"), v2);
        kv(c, _("Uso del canal"), line);
        if (n->uptime) {
            uint32_t u = n->uptime;
            if (u < 3600) snprintf(line, sizeof line, "%lu min", (unsigned long)(u / 60));
            else if (u < 172800) snprintf(line, sizeof line, "%lu h", (unsigned long)(u / 3600));
            else snprintf(line, sizeof line, "%lu d", (unsigned long)(u / 86400));
            kv(c, _("Encendido"), line);
        }
    }
    if (n->has_env) {
        msh_ui_num(v, sizeof v, n->temp, 1);
        msh_ui_num(v2, sizeof v2, n->hum, 0);
        snprintf(line, sizeof line, "%s °C  ·  %s %%", v, v2);
        if (n->press > 0) {
            size_t l = strlen(line);
            msh_ui_num(v, sizeof v, n->press, 0);
            snprintf(line + l, sizeof line - l, "  ·  %s hPa", v);
        }
        kv(c, _("Ambiente"), line);
    }
    if (n->has_pos) {
        char la[20], lo[20];
        snprintf(la, sizeof la, "%.5f", (double)(n->lat_i * 1e-7f));
        snprintf(lo, sizeof lo, "%.5f", (double)(n->lon_i * 1e-7f));
        snprintf(line, sizeof line, "%s, %s  ·  %ld m", la, lo, (long)n->alt);
        kv(c, _("Posición"), line);
        float d = msh_node_dist(n);
        if (d >= 0) {
            msh_node_t *mine = msh_node_find(MSH.my_num);
            msh_ui_dist(v, sizeof v, d);
            int brg = (int)(msh_bearing_deg(mine->lat_i, mine->lon_i, n->lat_i, n->lon_i) + 0.5f) % 360;
            snprintf(line, sizeof line, "%s  ·  %d°", v, brg);
            kv(c, _("Distancia"), line);
        }
    }
    if (n->has_pkc) kv(c, _("Mensajes directos"), _("cifrados con su clave pública"));
    if (lv_obj_get_child_count(c) == 0) lv_obj_delete(c);

    msh_tele_t *h = msh_tele_of(n->num, false);
    if (h && h->n >= 2) {
        msh_ui_label(col, _("Telemetría"), aos_font_small, AOS_C_DIM);
        chart(col, h, _("Batería"), "%", 0, MSH_T_BATT, g_batt, 0, NULL, 0, 100);
        chart(col, h, _("Tensión"), "V", 2, MSH_T_VOLT, g_volt, 0, NULL, 3, 4);
        chart(col, h, _("Uso del canal (verde) y aire (naranja)"), "%", 1, MSH_T_CHU, g_chu, MSH_T_AIR, g_air, 0, 10);
        chart(col, h, _("Temperatura"), "°C", 1, MSH_T_TEMP, g_temp, 0, NULL, 1e9f, -1e9f);
        chart(col, h, _("Humedad"), "%", 0, MSH_T_HUM, g_hum, 0, NULL, 1e9f, -1e9f);
        chart(col, h, _("Presión"), "hPa", 0, MSH_T_PRESS, g_press, 0, NULL, 1e9f, -1e9f);
    }
    msh_unlock();
}
