/*
 * P4OS - Modbus: the five tabs (modbus.c has the frame around them).
 */
#include "mb_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const FMT_NAME[FMT_COUNT] = { "U16", "S16", "HEX", "BIN", "U32", "S32", "Float" };

/* opens the link if it is closed: a tap on Send means "talk now" */
static void ensure_open(void)
{
    mb_lock();
    bool was = MB.want_open;
    MB.want_open = true;
    mb_unlock();
    if (!was) mb_bar_refresh();
}

static void fmt_time(char *o, size_t n, uint32_t us)
{
    if (us >= 10000000) snprintf(o, n, "%u s", (unsigned)(us / 1000000));
    else if (us >= 10000) snprintf(o, n, "%u ms", (unsigned)(us / 1000));
    else snprintf(o, n, "%u,%u ms", (unsigned)(us / 1000), (unsigned)(us / 100 % 10));
}

/* ======================================================================== */
/* Maestro                                                                   */
/* ======================================================================== */

static const struct { uint8_t fc; const char *name; } FCS[] = {
    { 1, N_("01 Coils") }, { 2, N_("02 Entradas") }, { 3, N_("03 Holding") }, { 4, N_("04 Input") },
    { 5, N_("05 Coil") }, { 6, N_("06 Registro") }, { 15, N_("15 Coils") }, { 16, N_("16 Registros") },
    { 8, N_("08 Eco") }, { 17, N_("17 Id") }, { 43, N_("43 Identificación") },
};
static const uint16_t PERIODS[] = { 100, 250, 500, 1000, 2000 };

static void m_changed(void)
{
    mb_settings_save();
    mb_rebuild_page();
}

static void m_fc_cb(lv_event_t *e)
{
    mb_lock();
    MB.mreq.fc = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (MB.mreq.fc >= 5) MB.m_live = false;     /* writing over and over is never what was meant */
    mb_unlock();
    m_changed();
}

static void m_unit_done(double v) { mb_lock(); MB.mreq.unit = (uint8_t)(v < 0 ? 0 : v > 247 ? 247 : v); mb_unlock(); m_changed(); }
static void m_addr_done(double v) { mb_lock(); MB.mreq.addr = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); mb_unlock(); m_changed(); }
static void m_n_done(double v)
{
    int max = MB.mreq.fc <= 2 ? 2000 : 125;
    mb_lock(); MB.mreq.n = (uint16_t)(v < 1 ? 1 : v > max ? max : v); mb_unlock();
    m_changed();
}
static void m_val_done(double v)
{
    mb_lock();
    MB.mreq.wv[0] = (uint16_t)(int32_t)v;
    MB.mreq.wn = 1;
    mb_unlock();
    m_changed();
}
static void m_unit_cb(lv_event_t *e) { (void)e; mb_keypad(_("Unidad (0 = broadcast, sin respuesta)"), MB.mreq.unit, 0, "", m_unit_done); }
static void m_addr_cb(lv_event_t *e) { (void)e; mb_keypad(_("Dirección inicial (desde 0)"), MB.mreq.addr, 0, "", m_addr_done); }
static void m_n_cb(lv_event_t *e) { (void)e; mb_keypad(_("Cantidad"), MB.mreq.n, 0, "", m_n_done); }
static void m_val_cb(lv_event_t *e)
{
    (void)e;
    if (MB.mreq.fc == 5) { mb_lock(); MB.mreq.wv[0] = !MB.mreq.wv[0]; mb_unlock(); m_changed(); return; }
    MBU.kp_hex = true;
    mb_keypad(_("Valor"), (int16_t)MB.mreq.wv[0], 0, "", m_val_done);
}

static void m_vals_done(const char *s)
{
    uint16_t v[123];
    int n = 0;
    const char *p = s;
    while (*p && n < 123) {
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char *end;
        long x = strtol(p, &end, 0);
        if (end == p) break;
        v[n++] = MB.mreq.fc == 15 ? (x ? 1 : 0) : (uint16_t)x;
        p = end;
    }
    if (!n) { aos_ui_toast(_("No entendí los valores"), 2000); return; }
    mb_lock();
    memcpy(MB.mreq.wv, v, (size_t)n * sizeof v[0]);
    MB.mreq.wn = (uint16_t)n;
    mb_unlock();
    m_changed();
}

static void m_vals_cb(lv_event_t *e)
{
    (void)e;
    char t[160] = "";
    size_t k = 0;
    for (int i = 0; i < MB.mreq.wn && k + 8 < sizeof t; i++) k += (size_t)snprintf(t + k, sizeof t - k, "%s%u", i ? " " : "", MB.mreq.wv[i]);
    mb_text_entry(MB.mreq.fc == 15 ? _("Coils a escribir, separadas por espacios (1 0 1 1)")
                                   : _("Registros a escribir, separados por espacios (100 0x1F -5)"), t, m_vals_done);
}

static void m_send_cb(lv_event_t *e)
{
    (void)e;
    ensure_open();
    mb_lock();
    MB.m_once = true;
    mb_unlock();
}

static void m_live_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (on) ensure_open();
    mb_lock();
    MB.m_live = on && MB.mreq.fc <= 4;
    mb_unlock();
}

static void m_per_cb(lv_event_t *e) { mb_lock(); MB.m_period_ms = PERIODS[(intptr_t)lv_event_get_user_data(e)]; mb_unlock(); m_changed(); }
static void m_fmt_cb(lv_event_t *e) { MBU.fmt = (int)(intptr_t)lv_event_get_user_data(e); m_changed(); }
static void m_swap_cb(lv_event_t *e) { (void)e; MBU.swap = !MBU.swap; m_changed(); }
static void m_clear_cb(lv_event_t *e) { (void)e; mb_bus_clear_stats(); }

static void m_tool_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock();
    bool busy = MB.tool != MB_TOOL_NONE;
    bool rtu = MB.cfg.tr == MB_TR_RTU;
    if (busy) MB.tool_stop = true;
    else if (rtu) { MB.tool = t; MB.tool_stop = false; MB.tool_pos = 0; MB.m_live = false; }
    mb_unlock();
    if (!rtu) { aos_ui_toast(_("Sólo por RTU"), 2000); return; }
    if (!busy) ensure_open();
    mb_rebuild_page();
}

/* the gateway: TCP clients on the network asking the units on this bus */
static void m_gw_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    mb_lock();
    MB.gw_on = on;
    MB.tcp_err[0] = 0;
    mb_unlock();
    if (on) ensure_open();
    m_changed();
}

static void m_gwp_done(double v)
{
    mb_lock();
    if (v >= 1 && v <= 65535) MB.gw_port = (uint16_t)v;
    bool on = MB.gw_on;
    MB.gw_on = false;                       /* the thread closes the old port... */
    mb_unlock();
    aos_hal_sleep_ms(120);
    mb_lock();
    MB.gw_on = on;                          /* ...and opens the new one */
    mb_unlock();
    m_changed();
}

static void m_gwp_cb(lv_event_t *e) { (void)e; mb_keypad(_("Puerto TCP de la pasarela"), MB.gw_port ? MB.gw_port : 502, 0, "", m_gwp_done); }

static void m_gateway(lv_obj_t *left, int32_t lw)
{
    mb_lock();
    bool on = MB.gw_on, rtu = MB.cfg.tr == MB_TR_RTU;
    uint16_t port = MB.gw_port ? MB.gw_port : 502;
    mb_unlock();
    if (!rtu) return;
    mb_section(left, _("PASARELA MODBUS TCP → RTU"));
    lv_obj_t *c = mb_card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_t *r = mb_box(c, lw - 32, 60);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 14, 0);
    aos_label(r, _("Atender clientes TCP"), aos_font_body, AOS_C_TEXT);
    mb_switch(r, on, m_gw_cb);
    char t[32];
    snprintf(t, sizeof t, "%s %u", _("Puerto"), port);
    mb_chip(r, t, false, m_gwp_cb, NULL);
    MBU.m_gw = aos_label(c, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(MBU.m_gw, lw - 32);
    lv_label_set_long_mode(MBU.m_gw, LV_LABEL_LONG_MODE_WRAP);
    mb_caption(c, _("Un programa de la PC (Modbus Poll, un SCADA, Home Assistant) pide por la red a esta placa, y ella le pregunta al equipo por el bus: la unidad del pedido es la del equipo. Si el equipo no contesta, el cliente recibe la excepción 0B. Anda con la conexión abierta."), lw - 32);
}

static void m_gw_fill(void)
{
    if (!MBU.m_gw) return;
    mb_lock();
    bool on = MB.gw_on, open = MB.open;
    uint32_t req = MB.gw_req, ok = MB.gw_ok, fail = MB.gw_fail;
    char peer[24], err[64];
    snprintf(peer, sizeof peer, "%s", MB.tcp_peer);
    snprintf(err, sizeof err, "%s", MB.tcp_err);
    uint16_t port = MB.gw_port ? MB.gw_port : 502;
    mb_unlock();
    const char *ip = aos_hal_net_ip();
    if (err[0]) { lv_label_set_text(MBU.m_gw, err); lv_obj_set_style_text_color(MBU.m_gw, AOS_C_ORANGE, 0); return; }
    lv_obj_set_style_text_color(MBU.m_gw, AOS_C_TEXT, 0);
    if (!on) lv_label_set_text(MBU.m_gw, _("Apagada."));
    else if (!open) lv_label_set_text(MBU.m_gw, _("Prendida: arranca al abrir la conexión."));
    else lv_label_set_text_fmt(MBU.m_gw, _("Escuchando en %s:%u · cliente: %s · %u pedidos, %u contestados, %u sin respuesta"),
                               ip && ip[0] ? ip : "?", port, peer[0] ? peer : _("ninguno"), (unsigned)req, (unsigned)ok, (unsigned)fail);
}

static void m_found_unit_cb(lv_event_t *e)
{
    mb_lock();
    MB.mreq.unit = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    mb_unlock();
    m_changed();
}

static void m_found_baud_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock();
    unsigned b = 0;
    char par = 'N';
    unsigned stop = 1;
    if (sscanf(MB.bauds_found[i], "%u 8%c%u", &b, &par, &stop) == 3) {
        MB.cfg.baud = b;
        MB.cfg.par = par;
        MB.cfg.stop = (uint8_t)stop;
    }
    mb_unlock();
    mb_bar_refresh();
    m_changed();
}

/* A tap on a value: a coil flips (FC 05), a holding register is written (FC 06). */
static uint16_t s_wr_addr;

static void m_write_done(double v)
{
    mb_lock();
    if (MB.m_wqn < 8) {
        mb_mreq_t *w = &MB.m_wq[MB.m_wqn++];
        memset(w, 0, sizeof *w);
        w->unit = MB.mreq.unit;
        w->fc = 6;
        w->addr = s_wr_addr;
        w->wv[0] = (uint16_t)(int32_t)v;
        w->wn = 1;
    }
    mb_unlock();
}

static void m_cell_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock();
    int fc = MB.m_got_fc;
    uint16_t a = (uint16_t)(MB.m_got_addr + i), v = MB.m_val[i];
    mb_unlock();
    if (fc == 1) {
        mb_lock();
        if (MB.m_wqn < 8) {
            mb_mreq_t *w = &MB.m_wq[MB.m_wqn++];
            memset(w, 0, sizeof *w);
            w->unit = MB.mreq.unit;
            w->fc = 5;
            w->addr = a;
            w->wv[0] = !v;
            w->wn = 1;
        }
        mb_unlock();
        ensure_open();
    } else if (fc == 3) {
        s_wr_addr = a;
        char t[48];
        snprintf(t, sizeof t, _("Escribir el registro %u (FC 06)"), a);
        MBU.kp_hex = true;
        mb_keypad(t, v, 0, "", m_write_done);
        ensure_open();
    }
}

static void val_format(int fmt, bool swap, const uint16_t *v, int i, int n, char *o, size_t len)
{
    switch (fmt) {
    case FMT_S16: snprintf(o, len, "%d", (int16_t)v[i]); break;
    case FMT_HEX: snprintf(o, len, "0x%04X", v[i]); break;
    case FMT_BIN: {
        size_t k = 0;
        for (int b = 15; b >= 0 && k + 2 < len; b--) {
            o[k++] = (v[i] >> b) & 1 ? '1' : '0';
            if (b && b % 4 == 0 && k + 2 < len) o[k++] = ' ';
        }
        o[k] = 0;
        break;
    }
    case FMT_U32: case FMT_S32: case FMT_FLOAT: {
        if (i + 1 >= n) { snprintf(o, len, "·"); break; }
        uint32_t w = swap ? ((uint32_t)v[i + 1] << 16 | v[i]) : ((uint32_t)v[i] << 16 | v[i + 1]);
        if (fmt == FMT_U32) snprintf(o, len, "%lu", (unsigned long)w);
        else if (fmt == FMT_S32) snprintf(o, len, "%ld", (long)(int32_t)w);
        else { float f; memcpy(&f, &w, 4); snprintf(o, len, "%.6g", (double)f); }
        break;
    }
    default: snprintf(o, len, "%u", v[i]); break;
    }
}

static void m_grid_fill(void)
{
    if (!MBU.m_grid) return;
    lv_obj_clean(MBU.m_grid);
    mb_lock();
    int rc = MB.m_rc, fc = MB.m_got_fc, n = MB.m_got_n;
    uint16_t a0 = MB.m_got_addr;
    uint32_t seq = MB.m_seq;
    static uint16_t v[2000];
    memcpy(v, MB.m_val, sizeof v);
    char text[192];
    snprintf(text, sizeof text, "%s", MB.m_text);
    mb_unlock();
    if (MBU.m_text) {
        bool show = seq && !rc && (fc == 8 || fc == 17 || fc == 43);
        lv_label_set_text(MBU.m_text, show ? text : "");
        if (show) lv_obj_remove_flag(MBU.m_text, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(MBU.m_text, LV_OBJ_FLAG_HIDDEN);
    }
    if (!seq || rc || fc > 4 || !n) return;
    bool bits = fc <= 2, wide = !bits && MBU.fmt >= FMT_U32;
    int32_t w = lv_obj_get_width(MBU.m_grid);
    int cols = bits ? (MBU.land ? 6 : 4) : (MBU.fmt == FMT_BIN ? (MBU.land ? 2 : 1) : (MBU.land ? 3 : 2));
    int32_t gap = 10, cw = (w - (cols - 1) * gap) / cols;
    int shown = 0, max = bits ? 256 : 125;
    for (int i = 0; i < n && shown < max; i += wide ? 2 : 1, shown++) {
        lv_obj_t *c = mb_card(MBU.m_grid, cw, 64);
        lv_obj_set_style_pad_hor(c, 14, 0);
        if (fc == 1 || fc == 3) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_add_event_cb(c, m_cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *al = lv_label_create(c);
        lv_obj_set_style_text_font(al, &aos_mono_18, 0);
        lv_obj_set_style_text_color(al, C_AMBER, 0);
        lv_label_set_text_fmt(al, "%u", a0 + i);
        lv_obj_align(al, LV_ALIGN_LEFT_MID, 0, 0);
        char t[40];
        if (bits) snprintf(t, sizeof t, "%s", v[i] ? "1" : "0");
        else val_format(MBU.fmt, MBU.swap, v, i, n, t, sizeof t);
        lv_obj_t *vl = lv_label_create(c);
        lv_obj_set_style_text_font(vl, &aos_mono_22, 0);
        lv_obj_set_style_text_color(vl, bits && v[i] ? AOS_C_GREEN : AOS_C_TEXT, 0);
        lv_label_set_text(vl, t);
        lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (n > shown && !wide) {
        char t[64];
        snprintf(t, sizeof t, _("y %d más"), n - shown);
        aos_label(MBU.m_grid, t, aos_font_caption, AOS_C_DIM);
    }
}

static void m_tool_fill(void)
{
    if (!MBU.m_found) return;
    mb_lock();
    int tool = MB.tool, pos = MB.tool_pos, total = MB.tool_total, nb = MB.nbauds_found, nclean = MB.nbauds_clean;
    uint8_t found[248];
    memcpy(found, MB.found, sizeof found);
    char bauds[8][24];
    memcpy(bauds, MB.bauds_found, sizeof bauds);
    mb_unlock();
    if (MBU.m_tool) {
        if (tool) lv_label_set_text_fmt(MBU.m_tool, tool == MB_TOOL_SCAN ? _("Buscando unidades: %d de %d…") : _("Probando velocidades: %d de %d…"), pos, total);
        else lv_label_set_text(MBU.m_tool, "");
    }
    if (MBU.m_tool_bar) {
        lv_bar_set_range(MBU.m_tool_bar, 0, total > 0 ? total : 1);
        lv_bar_set_value(MBU.m_tool_bar, pos, LV_ANIM_OFF);
        if (tool) lv_obj_remove_flag(MBU.m_tool_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(MBU.m_tool_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(MBU.m_found);
    int any = 0;
    for (int u = 1; u <= 247; u++) {
        if (!found[u]) continue;
        char t[24];
        snprintf(t, sizeof t, found[u] == 1 ? "U%d" : found[u] == 2 ? "U%d (exc)" : "U%d (CRC)", u);
        mb_chip(MBU.m_found, t, false, m_found_unit_cb, (void *)(intptr_t)u);
        any++;
    }
    for (int i = 0; i < nb; i++) { mb_chip(MBU.m_found, bauds[i], i < nclean, m_found_baud_cb, (void *)(intptr_t)i); any++; }
    if (nb > 1 && !tool) {
        /* At the right speed every format gets an answer (a UART hands over
         * a byte with a bad parity too); the line's errors tell them apart. */
        /* one format answered clean (at one speed or, on a line with no
         * speed of its own like the simulator's, at all of them) */
        bool one = nclean >= 1;
        const char *f0 = nclean ? strchr(bauds[0], ' ') : NULL;
        for (int i = 1; i < nclean && one; i++) {
            const char *fi = strchr(bauds[i], ' ');
            one = f0 && fi && !strncmp(f0, fi, 5);
        }
        lv_obj_t *c = mb_caption(MBU.m_found, one ? _("Lo marcado contestó sin errores de línea: ése es el formato. Los otros contestaron con bytes de paridad o de parada malos.")
                                                  : _("La velocidad es segura; el formato no salió claro (contestaron varios sin errores, o todos con errores). El manual del equipo dice cuál (el estándar pide 8E1)."),
                                 lv_obj_get_width(MBU.m_found));
        lv_obj_set_style_text_color(c, one ? AOS_C_GREEN : AOS_C_ORANGE, 0);
    }
    if (!any && !tool && pos > 0) aos_label(MBU.m_found, _("Nadie contestó."), aos_font_caption, AOS_C_ORANGE);
}

void tab_master_refresh(bool values)
{
    if (!MBU.m_status) return;
    mb_lock();
    int rc = MB.m_rc;
    uint32_t seq = MB.m_seq, rt = MB.m_rt_last;
    uint32_t req = MB.m_req, ans = MB.m_ans, tmo = MB.m_tmo, crc = MB.m_crc, exc = MB.m_exc, sum = MB.m_rt_sum, mx = MB.m_rt_max;
    bool open = MB.open;
    int tool = MB.tool;
    mb_unlock();
    if (tool != MBU.last_tool) { mb_rebuild_page(); return; }   /* its button says Stop, or no longer */
    if (!seq) lv_label_set_text(MBU.m_status, open ? _("Tocá Enviar.") : _("Enviar abre la conexión."));
    else if (rc) lv_label_set_text_fmt(MBU.m_status, "%s", mb_strerror(rc));
    else lv_label_set_text_fmt(MBU.m_status, "OK · %u ms", (unsigned)rt);
    lv_obj_set_style_text_color(MBU.m_status, !seq ? AOS_C_DIM : rc < 0 ? AOS_C_RED : rc > 0 ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    if (MBU.m_stats) {
        lv_label_set_text_fmt(MBU.m_stats, _("%u pedidos · %u respuestas · %u sin respuesta · %u CRC · %u excepciones · promedio %u ms, máximo %u ms"),
                              (unsigned)req, (unsigned)ans, (unsigned)tmo, (unsigned)crc, (unsigned)exc,
                              (unsigned)(ans ? sum / ans : 0), (unsigned)mx);
    }
    if (values) { m_grid_fill(); m_tool_fill(); }
    m_gw_fill();
}

static void m_tools(lv_obj_t *left, int32_t lw, int tool)
{
    lv_obj_t *r;
    mb_section(left, _("HERRAMIENTAS"));
    lv_obj_t *tc = mb_card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tc, 16, 0);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tc, 12, 0);
    r = mb_chips_row(tc, lw - 32);
    mb_pill(r, tool == MB_TOOL_SCAN ? AOS_SYM_STOP : AOS_SYM_MAGNIFY, tool == MB_TOOL_SCAN ? _("Detener") : _("Buscar unidades"),
            tool == MB_TOOL_SCAN ? AOS_C_RED : AOS_C_CARD2, m_tool_cb, (void *)MB_TOOL_SCAN);
    mb_pill(r, tool == MB_TOOL_BAUD ? AOS_SYM_STOP : AOS_SYM_SPEEDOMETER, tool == MB_TOOL_BAUD ? _("Detener") : _("Buscar velocidad"),
            tool == MB_TOOL_BAUD ? AOS_C_RED : AOS_C_CARD2, m_tool_cb, (void *)MB_TOOL_BAUD);
    MBU.m_tool = aos_label(tc, "", aos_font_caption, AOS_C_DIM);
    MBU.m_tool_bar = lv_bar_create(tc);
    lv_obj_set_size(MBU.m_tool_bar, lw - 32, 12);
    lv_obj_set_style_bg_color(MBU.m_tool_bar, C_TEAL, LV_PART_INDICATOR);
    MBU.m_found = mb_chips_row(tc, lw - 32);
    mb_caption(tc, _("Buscar unidades pregunta a las 1 a 247 por el holding 0. Buscar velocidad prueba ocho velocidades y cuatro formatos con la unidad elegida; tocá un resultado para usarlo."), lw - 32);

}

void tab_master_build(lv_obj_t *parent, int32_t ch)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    mb_columns(parent, ch, 600, &left, &lw, &right, &rw);
    mb_lock();
    mb_mreq_t q = MB.mreq;
    bool live = MB.m_live;
    uint16_t per = MB.m_period_ms;
    int tool = MB.tool;
    bool prof = MB.prof_on;
    mb_unlock();
    MBU.last_tool = tool;
    if (prof) {
        /* a profile: what the device has, read over and over */
        lv_obj_delete(left);
        if (right != left) lv_obj_delete(right);
        int32_t w = MBU.W - 2 * AOS_UI_PAD;
        lv_obj_t *col = mb_column(parent, w, ch);
        prof_chip(col, w);
        MBU.m_status = aos_label(col, "", aos_font_body, AOS_C_DIM);
        MBU.m_stats = mb_caption(col, "", w);
        prof_points(col, w, false);
        tab_master_refresh(true);
        return;
    }
    prof_chip(left, lw);

    mb_section(left, _("PEDIDO"));
    lv_obj_t *c = mb_card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_t *r = mb_chips_row(c, lw - 32);
    for (size_t i = 0; i < sizeof FCS / sizeof FCS[0]; i++) mb_chip(r, aos_tr(FCS[i].name), q.fc == FCS[i].fc, m_fc_cb, (void *)(intptr_t)FCS[i].fc);
    r = mb_chips_row(c, lw - 32);
    char t[64];
    snprintf(t, sizeof t, "%s %u", _("Unidad"), q.unit);
    mb_chip(r, t, false, m_unit_cb, NULL);
    if (q.fc != 8 && q.fc != 17 && q.fc != 43) {
        snprintf(t, sizeof t, "%s %u", _("Desde"), q.addr);
        mb_chip(r, t, false, m_addr_cb, NULL);
    }
    if (q.fc <= 4) {
        snprintf(t, sizeof t, "%s %u", _("Cantidad"), q.n);
        mb_chip(r, t, false, m_n_cb, NULL);
    } else if (q.fc == 5) {
        snprintf(t, sizeof t, "%s %s", _("Valor"), q.wv[0] ? "ON" : "OFF");
        mb_chip(r, t, false, m_val_cb, NULL);
    } else if (q.fc == 6 || q.fc == 8) {
        snprintf(t, sizeof t, q.fc == 8 ? "%s 0x%04X" : "%s %u", q.fc == 8 ? _("Dato") : _("Valor"), q.wv[0]);
        mb_chip(r, t, false, m_val_cb, NULL);
    } else if (q.fc == 15 || q.fc == 16) {
        size_t k = (size_t)snprintf(t, sizeof t, "%s:", _("Valores"));
        int wn = q.wn ? q.wn : 1;
        for (int i = 0; i < wn && k + 12 < sizeof t; i++) k += (size_t)snprintf(t + k, sizeof t - k, " %u", q.wv[i]);
        if (wn > 0 && k + 12 >= sizeof t) snprintf(t + k, sizeof t - k, " …");
        mb_chip(r, t, false, m_vals_cb, NULL);
    }
    lv_obj_t *r4 = mb_box(c, lw - 32, 76);
    lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r4, 14, 0);
    mb_pill(r4, AOS_SYM_SEND, _("Enviar"), C_TEAL, m_send_cb, NULL);
    if (q.fc <= 4) {
        aos_label(r4, _("En vivo"), aos_font_body, AOS_C_TEXT);
        mb_switch(r4, live, m_live_cb);
    }
    if (q.fc <= 4) {
        r = mb_chips_row(c, lw - 32);
        for (size_t i = 0; i < sizeof PERIODS / sizeof PERIODS[0]; i++) {
            snprintf(t, sizeof t, "%u ms", PERIODS[i]);
            mb_chip(r, t, per == PERIODS[i], m_per_cb, (void *)(intptr_t)i);
        }
    }

    if (MBU.land) { m_tools(left, lw, tool); m_gateway(left, lw); }
    mb_section(right, _("RESPUESTA"));
    MBU.m_status = aos_label(right, "", aos_font_body, AOS_C_DIM);
    MBU.m_stats = mb_caption(right, "", rw);
    lv_obj_t *sr = mb_chips_row(right, rw);
    mb_chip(sr, _("Borrar cuentas"), false, m_clear_cb, NULL);
    if (q.fc == 3 || q.fc == 4) {
        r = mb_chips_row(right, rw);
        for (int f = 0; f < FMT_COUNT; f++) mb_chip(r, FMT_NAME[f], MBU.fmt == f, m_fmt_cb, (void *)(intptr_t)f);
        if (MBU.fmt >= FMT_U32) mb_chip(r, MBU.swap ? "CDAB" : "ABCD", MBU.swap, m_swap_cb, NULL);
    }
    MBU.m_text = mb_caption(right, "", rw);
    lv_obj_set_style_text_font(MBU.m_text, aos_font_small, 0);
    lv_obj_set_style_text_color(MBU.m_text, AOS_C_TEXT, 0);
    MBU.m_grid = mb_box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(MBU.m_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(MBU.m_grid, 10, 0);
    if (q.fc == 1 || q.fc == 3) mb_caption(right, q.fc == 1 ? _("Tocá una coil para invertirla (FC 05).") : _("Tocá un registro para escribirlo (FC 06)."), rw);
    if (!MBU.land) { m_tools(left, lw, tool); m_gateway(left, lw); }
    tab_master_refresh(true);
}

/* ======================================================================== */
/* Esclavo                                                                   */
/* ======================================================================== */

static const char *const FAULT_NAME[MB_FAULT_COUNT] = { N_("Normal"), N_("Mudo"), N_("Lento"), N_("CRC malo"), N_("Ocupado (06)"), N_("Falla (04)") };

static void s_unit_done(double v) { mb_lock(); MB.s_unit = (uint8_t)(v < 1 ? 1 : v > 247 ? 247 : v); mb_unlock(); m_changed(); }
static void s_unit_cb(lv_event_t *e) { (void)e; mb_keypad(_("Contestar como la unidad"), MB.s_unit, 0, "", s_unit_done); }
static void s_fault_cb(lv_event_t *e) { mb_lock(); MB.s_fault = (int)(intptr_t)lv_event_get_user_data(e); mb_unlock(); m_changed(); }
static void s_delay_done(double v) { mb_lock(); MB.s_delay_ms = (uint16_t)(v < 0 ? 0 : v > 5000 ? 5000 : v); mb_unlock(); m_changed(); }
static void s_delay_cb(lv_event_t *e) { (void)e; mb_keypad(_("Demora de la respuesta"), MB.s_delay_ms, 0, "ms", s_delay_done); }
static void s_anim_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    mb_lock(); MB.s_anim = on; mb_unlock();
    mb_settings_save();
}
static void s_clear_cb(lv_event_t *e) { (void)e; mb_bus_clear_stats(); }

static int s_edit;
static void s_hr_done(double v) { mb_lock(); if (MB.s_hr) MB.s_hr[s_edit] = (uint16_t)(int32_t)v; MB.s_seq++; mb_unlock(); }
static void s_hr_cb(lv_event_t *e)
{
    s_edit = (int)(intptr_t)lv_event_get_user_data(e);
    char t[40];
    snprintf(t, sizeof t, _("Holding %d"), s_edit);
    MBU.kp_hex = true;
    mb_keypad(t, MB.s_hr ? MB.s_hr[s_edit] : 0, 0, "", s_hr_done);
}
static void s_coil_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock(); if (MB.s_coil) MB.s_coil[i] ^= 1; MB.s_seq++; mb_unlock();
}
static void s_di_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock(); if (MB.s_di) MB.s_di[i] ^= 1; MB.s_seq++; mb_unlock();
}

void tab_slave_refresh(void)
{
    if (!MBU.s_stats) return;
    mb_lock();
    uint32_t req = MB.s_req, mine = MB.s_mine, other = MB.s_other, bc = MB.s_bcast, crc = MB.s_crc, exc = MB.s_exc;
    char last[96];
    snprintf(last, sizeof last, "%s", MB.s_last);
    uint16_t hr[16], ir[8];
    uint8_t coil[16], di[8];
    if (MB.s_hr) { memcpy(hr, MB.s_hr, sizeof hr); memcpy(ir, MB.s_ir, sizeof ir); memcpy(coil, MB.s_coil, sizeof coil); memcpy(di, MB.s_di, sizeof di); }
    bool open = MB.open, tcp = MB.cfg.tr == MB_TR_TCP;
    char peer[24];
    snprintf(peer, sizeof peer, "%s", MB.tcp_peer);
    mb_role_t role = MB.role;
    mb_unlock();
    lv_label_set_text_fmt(MBU.s_stats, _("%u tramas oídas · %u para esta unidad · %u para otras · %u broadcast · %u CRC malos · %u excepciones"),
                          (unsigned)req, (unsigned)mine, (unsigned)other, (unsigned)bc, (unsigned)crc, (unsigned)exc);
    if (!open || role != MB_ROLE_SLAVE) lv_label_set_text(MBU.s_last, _("Abrí la conexión para que conteste."));
    else if (tcp) lv_label_set_text_fmt(MBU.s_last, "%s %s · %s %s", _("Cliente:"), peer[0] ? peer : _("ninguno todavía"), _("Último pedido:"), last[0] ? last : "—");
    else lv_label_set_text_fmt(MBU.s_last, "%s %s", _("Último pedido:"), last[0] ? last : "—");
    char t[16];
    for (int i = 0; i < 16; i++) {
        if (MBU.s_hr[i]) { snprintf(t, sizeof t, "%u", hr[i]); lv_label_set_text(MBU.s_hr[i], t); }
        if (MBU.s_coil[i]) {
            lv_label_set_text(MBU.s_coil[i], coil[i] ? "1" : "0");
            lv_obj_set_style_text_color(MBU.s_coil[i], coil[i] ? AOS_C_GREEN : AOS_C_DIM, 0);
        }
    }
    for (int i = 0; i < 8; i++) {
        if (MBU.s_ir[i]) { snprintf(t, sizeof t, "%u", ir[i]); lv_label_set_text(MBU.s_ir[i], t); }
        if (MBU.s_di[i]) {
            lv_label_set_text(MBU.s_di[i], di[i] ? "1" : "0");
            lv_obj_set_style_text_color(MBU.s_di[i], di[i] ? AOS_C_GREEN : AOS_C_DIM, 0);
        }
    }
}

static void s_grid(lv_obj_t *parent, int32_t w, const char *title, int count, int cols, lv_obj_t **vals, lv_event_cb_t cb)
{
    mb_section(parent, title);
    lv_obj_t *g = mb_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 8, 0);
    int32_t cw = (w - (cols - 1) * 8) / cols;
    for (int i = 0; i < count; i++) {
        lv_obj_t *c = mb_card(g, cw, 60);
        lv_obj_set_style_pad_hor(c, 12, 0);
        if (cb) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *al = lv_label_create(c);
        lv_obj_set_style_text_font(al, &aos_mono_18, 0);
        lv_obj_set_style_text_color(al, C_AMBER, 0);
        lv_label_set_text_fmt(al, "%d", i);
        lv_obj_align(al, LV_ALIGN_LEFT_MID, 0, 0);
        vals[i] = lv_label_create(c);
        lv_obj_set_style_text_font(vals[i], &aos_mono_22, 0);
        lv_obj_set_style_text_color(vals[i], AOS_C_TEXT, 0);
        lv_label_set_text(vals[i], "-");
        lv_obj_align(vals[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void tab_slave_build(lv_obj_t *parent, int32_t ch)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    mb_columns(parent, ch, 560, &left, &lw, &right, &rw);
    mb_lock();
    uint8_t unit = MB.s_unit;
    int fault = MB.s_fault;
    uint16_t delay = MB.s_delay_ms;
    bool anim = MB.s_anim;
    bool prof = MB.prof_on;
    mb_unlock();

    prof_chip(left, lw);
    mb_section(left, _("COMO CONTESTA"));
    lv_obj_t *c = mb_card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_t *r = mb_chips_row(c, lw - 32);
    char t[48];
    snprintf(t, sizeof t, "%s %u", _("Unidad"), unit);
    mb_chip(r, t, false, s_unit_cb, NULL);
    snprintf(t, sizeof t, "%s %u ms", _("Demora"), delay);
    mb_chip(r, t, false, s_delay_cb, NULL);
    r = mb_chips_row(c, lw - 32);
    for (int f = 0; f < MB_FAULT_COUNT; f++) mb_chip(r, aos_tr(FAULT_NAME[f]), fault == f, s_fault_cb, (void *)(intptr_t)f);
    if (!prof) {
        lv_obj_t *ar = mb_box(c, lw - 32, 60);
        lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ar, 14, 0);
        aos_label(ar, _("Valores que se mueven"), aos_font_body, AOS_C_TEXT);
        mb_switch(ar, anim, s_anim_cb);
    }
    mb_caption(c, _("Mudo no contesta; Lento contesta después de la demora; CRC malo manda la respuesta con el CRC roto; Ocupado y Falla contestan esas excepciones. Para ver cómo se las arregla el maestro."), lw - 32);

    MBU.s_last = aos_label(left, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(MBU.s_last, lw);
    lv_label_set_long_mode(MBU.s_last, LV_LABEL_LONG_MODE_WRAP);
    MBU.s_stats = mb_caption(left, "", lw);
    lv_obj_t *sr = mb_chips_row(left, lw);
    mb_chip(sr, _("Borrar cuentas"), false, s_clear_cb, NULL);
    if (prof) {
        mb_section(right, _("EL EQUIPO QUE HACE"));
        prof_points(right, rw, true);
        tab_slave_refresh();
        return;
    }
    mb_caption(left, _("Cada tabla va de 0 a 9999; fuera de eso contesta la excepción 02. Input 0: segundos; 1: un seno x1000; 2: una temperatura x10; 3: ruido; 4-5: un contador de 32 bits; 6-7: un float. Entradas 0 y 1 cambian solas. También contesta las funciones 08, 17 y 43."), lw);

    int cols = MBU.land ? 4 : 4;
    s_grid(right, rw, _("HOLDING 0-15 (TOCÁ PARA CAMBIAR)"), 16, cols, MBU.s_hr, s_hr_cb);
    s_grid(right, rw, _("COILS 0-15 (TOCÁ PARA INVERTIR)"), 16, cols * 2, MBU.s_coil, s_coil_cb);
    s_grid(right, rw, _("INPUT 0-7"), 8, cols, MBU.s_ir, NULL);
    s_grid(right, rw, _("ENTRADAS 0-7 (TOCÁ PARA INVERTIR)"), 8, cols * 2, MBU.s_di, s_di_cb);
    tab_slave_refresh();
}

/* ======================================================================== */
/* Monitor                                                                   */
/* ======================================================================== */

static void mon_clear_cb(lv_event_t *e) { (void)e; mb_bus_clear_stats(); }

void tab_mon_refresh(void)
{
    if (!MBU.mon_tot) return;
    mb_lock();
    uint32_t fr = MB.mon_frames, by = MB.mon_bytes, crc = MB.mon_crc, noise = MB.mon_noise, lerr = MB.mon_parity + MB.mon_frame;
    int nu = MB.nunits;
    static mb_unit_t u[MB_UNITS];
    memcpy(u, MB.units, sizeof u);
    bool open = MB.open;
    mb_role_t role = MB.role;
    mb_unlock();
    if (!open || role != MB_ROLE_MONITOR) lv_label_set_text(MBU.mon_tot, _("Abrí la conexión para escuchar."));
    else lv_label_set_text_fmt(MBU.mon_tot, _("%u tramas · %u bytes · %u con CRC malo · %u bytes de ruido · %u bytes con error de línea"), (unsigned)fr, (unsigned)by,
                               (unsigned)crc, (unsigned)noise, (unsigned)lerr);
    lv_obj_clean(MBU.mon_rows);
    int32_t w = lv_obj_get_width(MBU.mon_rows);
    for (int i = 0; i < nu; i++) {
        mb_unit_t *x = &u[i];
        lv_obj_t *c = mb_card(MBU.mon_rows, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 14, 0);
        bool bad = x->timeouts || x->crc || x->exc;
        lv_obj_t *h = aos_label(c, "", aos_font_body, bad ? AOS_C_ORANGE : AOS_C_GREEN);
        if (x->unit) lv_label_set_text_fmt(h, "%s %u", _("Unidad"), x->unit);
        else lv_label_set_text(h, _("Broadcast"));
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *l = aos_label(c, "", aos_font_caption, AOS_C_TEXT);
        lv_obj_set_width(l, w - 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        char last[64];
        uint8_t fake[6] = { x->unit, x->last_fc, (uint8_t)(x->last_addr >> 8), (uint8_t)x->last_addr, (uint8_t)(x->last_n >> 8), (uint8_t)x->last_n };
        mb_describe(fake, 6, true, last, sizeof last);
        lv_label_set_text_fmt(l, _("%u pedidos · %u respuestas · %u sin respuesta · %u excepciones · %u CRC\ndemora promedio %u ms, máxima %u ms\núltimo: %s"),
                              (unsigned)x->req, (unsigned)x->rsp, (unsigned)x->timeouts, (unsigned)x->exc, (unsigned)x->crc,
                              (unsigned)(x->rt_n ? x->rt_sum / x->rt_n : 0), (unsigned)x->rt_max, last);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 40);
    }
}

void tab_mon_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = MBU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = mb_column(parent, w, ch);
    mb_section(col, _("LO QUE PASA POR EL BUS"));
    MBU.mon_tot = aos_label(col, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(MBU.mon_tot, w);
    lv_label_set_long_mode(MBU.mon_tot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *r = mb_chips_row(col, w);
    mb_chip(r, _("Borrar cuentas"), false, mon_clear_cb, NULL);
    MBU.mon_rows = mb_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(MBU.mon_rows, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(MBU.mon_rows, 10, 0);
    mb_caption(col, _("Escucha sin transmitir nunca: el DE queda abajo, como si la placa no estuviera. Junta cada pedido con su respuesta; un pedido que se queda sin respuesta (otro pedido llega antes, o pasa un segundo) cuenta como sin respuesta de esa unidad. Cada trama, en Tráfico."), w);
    tab_mon_refresh();
}

/* ======================================================================== */
/* Tráfico                                                                   */
/* ======================================================================== */

#define T_ROWS 60

static void t_pause_cb(lv_event_t *e) { (void)e; MBU.paused = !MBU.paused; mb_rebuild_page(); }
static void t_err_cb(lv_event_t *e) { (void)e; MBU.only_err = !MBU.only_err; mb_rebuild_page(); }
static void t_clear_cb(lv_event_t *e) { (void)e; mb_bus_clear_log(); mb_rebuild_page(); }

const char *mb_note_text(int note)
{
    switch (note) {
    case MB_NOTE_TIMEOUT: return _("sin respuesta");
    case MB_NOTE_SILENT: return _("el esclavo se calla (falla elegida)");
    case MB_NOTE_LATE: return _("llegó fuera de un pedido");
    case MB_NOTE_OTHER_UNIT: return _("no es la respuesta esperada");
    case MB_NOTE_OPEN: return _("conexión abierta");
    case MB_NOTE_CLOSE: return _("conexión cerrada");
    default: return "";
    }
}

void tab_traffic_refresh(void)
{
    if (!MBU.t_list) return;
    mb_lock();
    uint32_t seq = MB.log_seq;
    uint64_t t0 = MB.t0_us;
    mb_unlock();
    int row = 0;
    static mb_log_t e;
    for (uint32_t k = seq; k-- > 0 && row < T_ROWS && seq - k <= MB_LOGN;) {
        if (!mb_log_get(k, &e)) break;
        bool err = (e.fl & (MB_LF_CRC | MB_LF_EXC | MB_LF_NOISE)) || e.note == MB_NOTE_TIMEOUT || e.note == MB_NOTE_SILENT;
        if (MBU.only_err && !err) continue;
        lv_obj_t *box = MBU.t_rows[row++];
        lv_obj_t *hdr = lv_obj_get_child(box, 0), *desc = lv_obj_get_child(box, 1);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
        char ts[24], gap[24] = "";
        uint64_t dt = e.t_us > t0 ? e.t_us - t0 : 0;
        snprintf(ts, sizeof ts, "%u,%03u", (unsigned)(dt / 1000000), (unsigned)(dt / 1000 % 1000));
        const uint8_t *d = e.d;
        int len = e.len;
        bool tcp = e.fl & MB_LF_TCP;
        char hex[3 * 48 + 8];
        size_t hk = 0;
        for (int i = 0; i < len && i < 48; i++) hk += (size_t)snprintf(hex + hk, sizeof hex - hk, "%02X ", d[i]);
        if (len > 48) snprintf(hex + hk, sizeof hex - hk, "… (%d)", len);
        else if (hk) hex[hk - 1] = 0;
        else hex[0] = 0;
        char what[128];
        lv_color_t col = AOS_C_TEXT;
        if (e.fl & MB_LF_NOTE) {
            snprintf(what, sizeof what, "%s", mb_note_text(e.note));
            if (e.note == MB_NOTE_TIMEOUT && len >= 2) snprintf(what, sizeof what, "%s: U%u %s", mb_note_text(e.note), d[0], mb_fc_name(d[1]));
            lv_label_set_text_fmt(hdr, "%s  --", ts);
            col = e.note == MB_NOTE_TIMEOUT || e.note == MB_NOTE_SILENT ? AOS_C_ORANGE : AOS_C_DIM;
        } else {
            if (e.fl & MB_LF_NOISE) snprintf(what, sizeof what, "%s", _("ruido: bytes que no forman una trama"));
            else if (e.fl & MB_LF_CRC) {
                char tmp[96];
                mb_describe(d, len, e.fl & MB_LF_REQ, tmp, sizeof tmp);
                snprintf(what, sizeof what, "%s · %s", _("CRC MALO"), tmp);
            } else if (tcp && len > 7) {
                uint8_t f[MB_MAXF];
                f[0] = d[6];
                memcpy(f + 1, d + 7, (size_t)(len - 7));
                mb_describe(f, len - 6, e.fl & MB_LF_REQ, what, sizeof what);
            } else mb_describe(d, len, (e.fl & MB_LF_REQ) != 0, what, sizeof what);
            if (e.note) { size_t l = strlen(what); snprintf(what + l, sizeof what - l, " (%s)", mb_note_text(e.note)); }
            if (e.gap_us) { char g[16]; fmt_time(g, sizeof g, e.gap_us); snprintf(gap, sizeof gap, " +%s", g); }
            if ((e.fl & MB_LF_RSP) && e.rt_ms) { size_t l = strlen(what); snprintf(what + l, sizeof what - l, _(" · contestó en %u ms"), e.rt_ms); }
            lv_label_set_text_fmt(hdr, "%s%s %s %s", ts, gap, e.fl & MB_LF_TX ? "TX" : "RX", hex);
            col = (e.fl & (MB_LF_CRC | MB_LF_NOISE)) ? AOS_C_RED : (e.fl & MB_LF_EXC) ? AOS_C_ORANGE : (e.fl & MB_LF_TX) ? AOS_C_TEAL : AOS_C_TEXT;
        }
        lv_obj_set_style_text_color(hdr, col, 0);
        lv_label_set_text(desc, what);
    }
    for (; row < T_ROWS; row++) if (MBU.t_rows[row]) lv_obj_add_flag(MBU.t_rows[row], LV_OBJ_FLAG_HIDDEN);
}

void tab_traffic_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = MBU.W - 2 * AOS_UI_PAD;
    lv_obj_t *r = mb_chips_row(parent, w);
    mb_chip(r, MBU.paused ? _("Seguir") : _("Pausa"), MBU.paused, t_pause_cb, NULL);
    mb_chip(r, _("Sólo errores"), MBU.only_err, t_err_cb, NULL);
    mb_chip(r, _("Borrar"), false, t_clear_cb, NULL);
    lv_obj_update_layout(r);
    int32_t rh = lv_obj_get_height(r) + 10;
    MBU.t_list = mb_column(parent, w, ch - rh);
    lv_obj_set_y(MBU.t_list, rh);
    lv_obj_set_style_pad_row(MBU.t_list, 8, 0);
    lv_obj_add_flag(MBU.t_list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < T_ROWS; i++) {
        lv_obj_t *b = mb_card(MBU.t_list, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(b, 10, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(b, 2, 0);
        lv_obj_t *h = lv_label_create(b);
        lv_obj_set_style_text_font(h, &aos_mono_18, 0);
        lv_obj_set_width(h, w - 20);
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_t *d = aos_label(b, "", aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(d, w - 20);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        MBU.t_rows[i] = b;
    }
    mb_caption(MBU.t_list, _("Lo más nuevo arriba. Los tiempos son de cuando los bytes llegaron a la app (unos caracteres después de pasar por el cable); \"+\" es el silencio antes de la trama. El botón BOOT pausa."), w);
    tab_traffic_refresh();
}

/* ======================================================================== */
/* Cableado                                                                  */
/* ======================================================================== */

static lv_obj_t *w_rect(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c)
{
    lv_obj_t *o = mb_box(p, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *w_text(lv_obj_t *p, int32_t x, int32_t y, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(p, t, f, c);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* The bus: three nodes on A and B, 120 ohm at the two ends. */
static void w_bus(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *d = mb_card(parent, w, 250);
    int32_t x0 = 70, x1 = w - 70, ya = 150, yb = 186;
    w_rect(d, x0, ya, x1 - x0, 5, AOS_C_YELLOW);
    w_rect(d, x0, yb, x1 - x0, 5, AOS_C_DIM);
    w_text(d, 18, ya - 12, "A", aos_font_small, AOS_C_YELLOW);
    w_text(d, 18, yb - 12, "B", aos_font_small, AOS_C_DIM);
    /* the terminators */
    for (int k = 0; k < 2; k++) {
        int32_t x = k ? x1 - 6 : x0;
        w_rect(d, x, ya, 6, yb - ya + 5, AOS_C_ORANGE);
        w_text(d, k ? x - 70 : x + 12, yb + 14, "120 Ω", aos_font_caption, AOS_C_ORANGE);
    }
    static const char *const NODE[3] = { "P4", N_("equipo"), N_("equipo") };
    for (int i = 0; i < 3; i++) {
        int32_t cx = x0 + 60 + i * (x1 - x0 - 120) / 2;
        w_rect(d, cx - 8, 90, 3, ya - 90, AOS_C_YELLOW);
        w_rect(d, cx + 6, 90, 3, yb - 90, AOS_C_DIM);
        lv_obj_t *n = w_rect(d, cx - 70, 26, 140, 64, i ? AOS_C_CARD2 : C_TEAL);
        lv_obj_set_style_radius(n, 12, 0);
        lv_obj_center(aos_label(n, i ? aos_tr(NODE[i]) : NODE[i], aos_font_small, AOS_C_TEXT));
    }
}

void tab_wire_build(lv_obj_t *parent, int32_t ch)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    mb_columns(parent, ch, 600, &left, &lw, &right, &rw);
    mb_lock();
    mb_cfg_t c = MB.cfg;
    mb_unlock();
    const aos_io_port_t *p = aos_io_port_find(c.port);
    mb_section(left, _("DEL HEADER AL TRANSCEPTOR"));
    lv_obj_t *card = mb_card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    if (c.tr == MB_TR_RTU && p && p->kind == AOS_PORT_UART) {
        static const char *const SIG[3] = { "TX", "RX", "DE" };
        static const char *const TO[3] = { N_("→ TXD del módulo (DI en un transceptor suelto)"), N_("← RXD del módulo (RO)"),
                                           N_("→ DE y /RE unidos; libre con un módulo automático") };
        for (int i = 0; i < 3; i++) {
            if (p->pins[i] < 0) continue;
            const aos_io_pin_t *hp = aos_io_pin_of_gpio(p->pins[i]);
            char t[96];
            snprintf(t, sizeof t, "%s  GPIO%d (pin %d)  %s", SIG[i], p->pins[i], hp ? hp->pin : 0, aos_tr(TO[i]));
            aos_label(card, t, aos_font_small, i == 2 ? C_AMBER : AOS_C_TEXT);
        }
        /* the 3V3 and the GND closest to the port's pins */
        const aos_io_pin_t *hdr = aos_io_header(), *rx = aos_io_pin_of_gpio(p->pins[1]);
        int near = rx ? rx->pin : 20, best3 = 0, bestg = 0;
        for (int i = 0; i < 40; i++) {
            const char *lb = hdr[i].label ? hdr[i].label : "";
            int dist = abs(hdr[i].pin - near);
            if (!strcmp(lb, "3V3") && (!best3 || dist < abs(best3 - near))) best3 = hdr[i].pin;
            if (!strcmp(lb, "GND") && (!bestg || dist < abs(bestg - near))) bestg = hdr[i].pin;
        }
        char t[64];
        snprintf(t, sizeof t, "3V3  (pin %d)  → VCC", best3);
        aos_label(card, t, aos_font_small, AOS_C_TEXT);
        snprintf(t, sizeof t, "GND  (pin %d)  → GND", bestg);
        aos_label(card, t, aos_font_small, AOS_C_TEXT);
        if (p->pins[2] < 0) mb_caption(card, _("Este puerto no tiene DE: sirve con un módulo de dirección automática (sólo TX, RX, VCC y GND)."), lw - 36);
    } else if (c.tr == MB_TR_RTU) {
        mb_caption(card, _("Un adaptador USB-RS485 (CH340 o FT232 con un transceptor automático) en el host USB de la placa: A, B y GND al bus; nada más que cablear."), lw - 36);
    } else {
        mb_caption(card, _("Por TCP no hay cables: el equipo está en la red."), lw - 36);
    }
    mb_section(left, _("EL BUS"));
    w_bus(left, lw);
    mb_section(right, _("LO QUE HAY QUE SABER"));
    static const char *const NOTES[] = {
        N_("Un módulo \"RS485 to TTL\" automático (VCC, TXD, RXD, GND; sin DE) cambia de dirección solo: alcanza con TX, RX y alimentarlo con 3,3 V. Si el LED TXD no parpadea cuando la placa manda, invertí TXD y RXD: con 3,3 V de los dos lados no se rompe nada."),
        N_("Alimentá el módulo con 3,3 V, no con 5 V: a 5 V su salida pone 5 V en el header, y los pines del P4 no lo aguantan. El MAX485 suelto es de 5 V; los de 3,3 V son MAX3485, SP3485, THVD1420."),
        N_("Con un transceptor que tiene DE, DE y /RE van juntos al pin DE: el P4 lo sube mientras transmite y lo baja para escuchar. Como monitor no transmite nunca."),
        N_("120 Ω sólo en las dos puntas del bus, no en cada equipo (muchos módulos ya traen una, marcada 121). Con todo apagado, entre A y B se miden unos 60 Ω (las dos en paralelo)."),
        N_("A y B: el estándar dice A = D− y B = D+, pero muchos fabricantes los rotulan al revés. Si nadie contesta, invertirlos no rompe nada."),
        N_("Polarización: una resistencia de A a 3,3 V y otra de B a GND (unos 680 Ω), en un solo lugar del bus, dejan la línea quieta en 1 cuando nadie habla. Muchos módulos ya las traen; sin ellas el monitor ve ruido entre tramas."),
        N_("GND común entre los equipos (el tercer cable), sobre todo con fuentes distintas: RS485 aguanta diferencias de unos pocos volts, no más."),
        N_("Par trenzado para A y B. A 9600 baudios el bus puede tener cientos de metros; a 115200, mucho menos."),
    };
    for (size_t i = 0; i < sizeof NOTES / sizeof NOTES[0]; i++) {
        lv_obj_t *n = mb_card(right, rw, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(n, 16, 0);
        lv_obj_t *l = aos_label(n, aos_tr(NOTES[i]), aos_font_caption, AOS_C_TEXT);
        lv_obj_set_width(l, rw - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
}
