/*
 * P4OS - Logic: the bar, the status line, the tools and their sheets.
 *
 *   top      the analyser (tap: choose another, look for SUMP ones), the
 *            rate and the depth (tap: the sampling sheet), the trigger,
 *            repeat, and Capture / Stop
 *   status   what the job is doing, or the last result; the capture and
 *            the cursors
 *   middle   the view (la_view.c)
 *   bottom   Channels, Decoders, Table, Measure, Files, and the zoom
 *
 * Every sheet covers the app and is closed with its X or "back". A sheet
 * that changed the decoders asks for a decode when it closes.
 */
#include "la.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SH_NONE = 0, SH_DEV, SH_SAMPLING, SH_TRIG, SH_CHANNELS, SH_CHANNEL, SH_DECS, SH_DEC, SH_TABLE, SH_MEAS, SH_FILES, SH_TEXT };

static struct {
    lv_obj_t *root, *sheet;
    int W, H;
    bool land;
    int sheet_kind, sheet_arg;
    bool dec_changed;
    lv_obj_t *dev_dot, *dev_name, *dev_detail;
    lv_obj_t *rate_lbl, *samp_lbl, *trig_lbl, *rep_btn, *cap_btn, *cap_lbl, *cap_ico;
    lv_obj_t *status, *info;
    lv_obj_t *dur_lbl, *meas_lbl;
    int tab_page, tab_filter;
    int meas_ch;
    char del_name[64];
    uint32_t del_at;
} U = { .meas_ch = 0, .tab_filter = -1 };

/* ---- widgets ---- */

static lv_obj_t *btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 72);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 8, 0);
    if (glyph) {
        lv_obj_t *g = lv_label_create(b);
        lv_obj_set_style_text_font(g, &aos_sym_28, 0);
        lv_label_set_text(g, glyph);
    }
    if (text) {
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_label_set_text(l, text);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_min_width(b, 60, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_bg_color(b, on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* a row of pills, one of them chosen; set(idx) is called on a tap */
typedef void (*set_fn)(int idx);

static void choice_cb(lv_event_t *e)
{
    lv_obj_t *p = lv_event_get_target(e);
    lv_obj_t *row = lv_obj_get_parent(p);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    set_fn set = (set_fn)lv_obj_get_user_data(row);
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++)
        lv_obj_set_style_bg_color(lv_obj_get_child(row, (int32_t)i), (int)i == idx ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    if (set) set(idx);
}

static lv_obj_t *choices(lv_obj_t *parent, const char *title, const char *const *labels, int n, int cur, set_fn set)
{
    lv_obj_t *c = card(parent);
    if (title) aos_label(c, title, aos_font_small, AOS_C_DIM);
    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)set);
    for (int i = 0; i < n; i++) pill(row, labels[i], i == cur, choice_cb, (void *)(intptr_t)i);
    return c;
}

/* ---- sheets ---- */

static void sheet_close_cb(lv_event_t *e)
{
    (void)e;
    la_sheet_close();
}

bool la_sheet_is_open(void) { return U.sheet != NULL; }

void la_sheet_close(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = NULL;
    U.dur_lbl = U.meas_lbl = NULL;
    int was = U.sheet_kind;
    U.sheet_kind = SH_NONE;
    if (was != SH_TEXT) la_cfg_save();
    if (U.dec_changed) {
        U.dec_changed = false;
        la_view_dirty();
        if (LA.cap.n && !la_decode_start()) la_set_msg(true, "%s", _("Ocupada: se decodifica con la próxima captura"));
    }
    la_ui_status();
}

lv_obj_t *la_sheet_open(const char *title)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.sheet);
    lv_obj_set_size(U.sheet, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = aos_label(U.sheet, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, U.W - 2 * AOS_UI_PAD - 100);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *x = btn(U.sheet, AOS_SYM_CLOSE, NULL, sheet_close_cb, NULL);
    lv_obj_set_width(x, 80);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 10);

    lv_obj_t *body = lv_obj_create(U.sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, U.W, U.H - 96);
    lv_obj_set_pos(body, 0, 96);
    lv_obj_set_style_pad_hor(body, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(body, 24, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    return body;
}

static lv_obj_t *sheet(int kind, int arg, const char *title)
{
    lv_obj_t *b = la_sheet_open(title);
    U.sheet_kind = kind;
    U.sheet_arg = arg;
    return b;
}

/* text entry */
static void (*s_text_done)(const char *);
static lv_obj_t *s_text_ta;

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[64];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(s_text_ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    la_sheet_close();
    if (done) done(v);
}

static void text_entry(const char *title, const char *value, int maxlen, void (*done)(const char *))
{
    lv_obj_t *body = sheet(SH_TEXT, 0, title);
    s_text_done = done;
    s_text_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(s_text_ta, true);
    lv_textarea_set_max_length(s_text_ta, (uint32_t)maxlen);
    lv_textarea_set_text(s_text_ta, value);
    lv_obj_set_size(s_text_ta, LV_PCT(100), 88);
    lv_obj_set_style_text_font(s_text_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(s_text_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(s_text_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(s_text_ta, 0, 0);
    lv_obj_set_style_radius(s_text_ta, 20, 0);
    lv_obj_set_style_pad_all(s_text_ta, 24, 0);
    lv_obj_t *kb = lv_keyboard_create(U.sheet);
    lv_obj_set_size(kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_textarea(kb, s_text_ta);
    lv_obj_add_event_cb(kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the analyser ---- */

static void dev_sheet(void);

static void dev_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (LA.busy || i < 0 || i >= LA.ndevs) return;
    LA.dev = i;
    aos_hal_pref_set_i32("la_drv", LA.devs[i].drv);
    /* a rate the new one does not have: the nearest it has */
    int n;
    const uint32_t *r = la_rates(&LA.devs[i], &n);
    uint32_t best = r[0];
    for (int k = 0; k < n; k++)
        if (r[k] <= LA.cfg.rate) best = r[k];
    LA.cfg.rate = best;
    dev_sheet();
    la_ui_status();
}

static void scene_set(int i)
{
    LA.cfg.demo = (uint8_t)i;
    la_demo_decoders(i);
    U.dec_changed = false;
    la_view_dirty();
}

static void probe_cb(lv_event_t *e)
{
    (void)e;
    if (!la_probe_start()) return;
    la_set_msg(false, "%s", _("Preguntando a los puertos serie USB..."));
    la_sheet_close();
}

static char s_probe_port[16];
static uint32_t s_probe_baud;

static void probe_port_job(void *arg)
{
    (void)arg;
    la_dev_t d;
    if (la_sump_probe(s_probe_port, s_probe_baud, &d)) {
        aos_hal_pref_set_str("la_sport", d.port);
        aos_hal_pref_set_i32("la_sbaud", (int32_t)d.baud);
        aos_hal_pref_set_str("la_sname", d.name);
        aos_hal_pref_set_i32("la_srate", (int32_t)d.max_rate);
        aos_hal_pref_set_i32("la_smem", (int32_t)d.max_samples);
        la_set_msg(false, _("SUMP: %s en %s"), d.name, d.port);
        la_prefer = LA_DRV_SUMP;
    } else {
        la_set_msg(true, _("Nadie contestó SUMP en %s a %u"), s_probe_port, (unsigned)s_probe_baud);
    }
    la_rescan = true;
}

static void probe_uart_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_probe_port, sizeof s_probe_port, "%s", v & 1 ? "uart.b" : "uart.a");
    s_probe_baud = v & 2 ? 921600 : 115200;
    if (la_job_start(LA_ST_CONNECT, probe_port_job, NULL)) la_sheet_close();
}

static void dev_sheet(void)
{
    lv_obj_t *b = sheet(SH_DEV, 0, _("Analizador"));
    for (int i = 0; i < LA.ndevs; i++) {
        const la_dev_t *d = &LA.devs[i];
        lv_obj_t *c = card(b);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, dev_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i == LA.dev) {
            lv_obj_set_style_border_color(c, AOS_C_ACCENT, 0);
            lv_obj_set_style_border_width(c, 3, 0);
        }
        aos_make_decorative(aos_label(c, d->name, aos_font_body, AOS_C_TEXT));
        lv_obj_t *l = note(c, d->detail);
        aos_make_decorative(l);
    }
    if (LA.devs[LA.dev].drv == LA_DRV_DEMO) {
        const char *sc[3];
        for (int i = 0; i < 3; i++) sc[i] = la_demo_scene_name(i);
        choices(b, _("Escena de la demo (pone sus decodificadores)"), sc, 3, LA.cfg.demo, scene_set);
    }
    lv_obj_t *row = lv_obj_create(b);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    btn(row, AOS_SYM_USB, _("Buscar SUMP en USB"), probe_cb, NULL);
    static const char *const UL[4] = { "uart.a 115200", "uart.b 115200", "uart.a 921600", "uart.b 921600" };
    for (int i = 0; i < 4; i++) btn(row, AOS_SYM_SERIAL_PORT, UL[i], probe_uart_cb, (void *)(intptr_t)i);
    lv_obj_t *c = card(b);
    if (!aos_hal_usb_host_on())
        aos_label(c, _("El host USB está apagado: prendelo en Ajustes, USB."), aos_font_small, AOS_C_ORANGE);
    note(c, _("Analizador USB de 24 MHz y 8 canales (Saleae Logic y sus clones, y las demás placas FX2): enchufalo al "
              "host USB de la placa (pines 25/27 para alta velocidad; 21/23 llega hasta 1 MHz). La primera vez hay que "
              "copiar el firmware libre fx2lafw de sigrok a la carpeta logica/fx2lafw de la tarjeta "
              "(fx2lafw-saleae-logic.fw), o subirlo desde la página Lógica del portal."));
    note(c, _("SUMP: el firmware de la app para un ESP32-S3, C3 o C6 (carpeta firmware de la app), un Open Bench "
              "Logic Sniffer u otro compatible, por USB o por una UART del header. Sólo a 3,3 V."));
}

/* ---- sampling ---- */

static uint32_t s_rate_v[24], s_samp_v[16];

static void dur_update(void)
{
    if (!U.dur_lbl) return;
    char d[24], b[96];
    la_fmt_time(LA.cfg.rate ? (float)LA.cfg.samples / (float)LA.cfg.rate : 0, d, sizeof d);
    snprintf(b, sizeof b, _("Duración de la captura: %s"), d);
    lv_label_set_text(U.dur_lbl, b);
}

static void rate_set(int i) { LA.cfg.rate = s_rate_v[i]; dur_update(); la_ui_status(); }
static void samp_set(int i) { LA.cfg.samples = s_samp_v[i]; dur_update(); la_ui_status(); }
static void pre_set(int i)
{
    static const uint8_t P[] = { 0, 10, 25, 50, 75, 90 };
    LA.cfg.pre = P[i];
}

static void sampling_sheet(void)
{
    lv_obj_t *b = sheet(SH_SAMPLING, 0, _("Muestreo"));
    const la_dev_t *d = &LA.devs[LA.dev];
    int n;
    const uint32_t *r = la_rates(d, &n);
    char lab[24][16];
    const char *lp[24];
    int cur = 0;
    for (int i = 0; i < n && i < 24; i++) {
        s_rate_v[i] = r[i];
        la_fmt_rate(r[i], lab[i], sizeof lab[i]);
        lp[i] = lab[i];
        if (r[i] == LA.cfg.rate) cur = i;
    }
    choices(b, _("Velocidad (al menos 4 veces la señal más rápida)"), lp, n, cur, rate_set);

    static const uint32_t S[] = { 10000, 50000, 100000, 250000, 500000, 1000000, 2000000, 4000000, 8000000, 16000000 };
    uint32_t max = la_max_samples();     /* it counts the capture in view as given back */
    if (d->max_samples && d->max_samples < max) max = d->max_samples;
    int k = 0;
    cur = -1;
    for (size_t i = 0; i < sizeof S / sizeof S[0] && k < 15; i++)
        if (S[i] <= max) s_samp_v[k++] = S[i];
    if (d->max_samples && (k == 0 || s_samp_v[k - 1] < d->max_samples) && d->max_samples <= max) s_samp_v[k++] = d->max_samples;
    char sl[16][16];
    const char *sp[16];
    for (int i = 0; i < k; i++) {
        la_fmt_count(s_samp_v[i], sl[i], sizeof sl[i]);
        sp[i] = sl[i];
        if (s_samp_v[i] == LA.cfg.samples) cur = i;
    }
    if (cur < 0 && k) {
        cur = k - 1;
        for (int i = 0; i < k; i++)
            if (s_samp_v[i] >= LA.cfg.samples) { cur = i; break; }
        LA.cfg.samples = s_samp_v[cur];
    }
    char mx[16], t[96];
    la_fmt_count(max, mx, sizeof mx);
    snprintf(t, sizeof t, d->max_samples ? _("Muestras (el analizador guarda hasta %s)") : _("Muestras (la memoria de la placa da hasta %s)"), mx);
    lv_obj_t *c = choices(b, t, sp, k, cur, samp_set);
    U.dur_lbl = note(c, "");
    dur_update();

    static const char *const P[] = { "0 %", "10 %", "25 %", "50 %", "75 %", "90 %" };
    static const uint8_t PV[] = { 0, 10, 25, 50, 75, 90 };
    int pc = 1;
    for (int i = 0; i < 6; i++)
        if (PV[i] == LA.cfg.pre) pc = i;
    choices(b, _("Antes del disparo"), P, 6, pc, pre_set);
    if (d->drv == LA_DRV_FX2)
        note(b, _("El FX2 manda todo en vivo: a 24 MHz son 24 MB/s por el USB. Si la captura sale con huecos, bajá la "
                  "velocidad (12 MHz anda holgado)."));
}

/* ---- trigger ---- */

static void trig_sheet(void);
static int s_trig_ch;

static void trig_set_one(int i) { LA.cfg.trig[s_trig_ch] = (uint8_t)i; la_ui_status(); }

static void trig_pick_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int ch = v >> 4, t = v & 15;
    LA.cfg.trig[ch] = (uint8_t)t;
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target(e));
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 1; i < n; i++)
        lv_obj_set_style_bg_color(lv_obj_get_child(row, (int32_t)i), (int)i - 1 == t ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    la_ui_status();
}

static const char *const TRIG_NAME[LA_T_COUNT] = { "X", "0", "1", N_("Sube"), N_("Baja"), N_("Cambia") };

static void trig_clear_cb(lv_event_t *e)
{
    (void)e;
    memset(LA.cfg.trig, LA_T_X, sizeof LA.cfg.trig);
    trig_sheet();
    la_ui_status();
}

static void trig_sheet(void)
{
    lv_obj_t *b = sheet(SH_TRIG, 0, _("Disparo"));
    note(b, _("La captura arranca cuando se cumplen todas las condiciones juntas. X: no importa. Un analizador SUMP "
              "que no sea el de la app sólo entiende niveles: Sube espera el 1 y Baja el 0."));
    for (int ch = 0; ch < LA_NCH; ch++) {
        lv_obj_t *row = lv_obj_create(b);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 8, 0);
        char nm[24];
        snprintf(nm, sizeof nm, "D%d %s", ch, LA.chname[ch]);
        lv_obj_t *l = aos_label(row, nm, aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(l, 170);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        for (int t = 0; t < LA_T_COUNT; t++)
            pill(row, _(TRIG_NAME[t]), LA.cfg.trig[ch] == t, trig_pick_cb, (void *)(intptr_t)(ch << 4 | t));
    }
    btn(b, AOS_SYM_DELETE, _("Sin disparo"), trig_clear_cb, NULL);
}

/* ---- channels ---- */

static void channels_sheet(void);

static void ch_show_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    LA.chshow[ch] = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    int n = 0;
    for (int i = 0; i < LA_NCH; i++) n += LA.chshow[i];
    if (!n) {
        LA.chshow[ch] = true;
        lv_obj_add_state(lv_event_get_target(e), LV_STATE_CHECKED);
    }
    la_view_dirty();
}

static void rename_done(const char *v)
{
    snprintf(LA.chname[U.sheet_arg], sizeof LA.chname[0], "%s", v);
    la_cfg_save();
    la_view_dirty();
    la_ui_channel_sheet(U.sheet_arg);
}

static void rename_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    char t[48];
    snprintf(t, sizeof t, _("Nombre de D%d"), ch);
    char cur[16];
    snprintf(cur, sizeof cur, "%s", LA.chname[ch]);
    text_entry(t, cur, 11, rename_done);
    U.sheet_arg = ch;
}

static void open_channel_cb(lv_event_t *e) { la_ui_channel_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *switch_row(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *row = card(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = aos_label(row, text, aos_font_body, AOS_C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 90, 48);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    return row;
}

static void channels_sheet(void)
{
    lv_obj_t *b = sheet(SH_CHANNELS, 0, _("Canales"));
    for (int ch = 0; ch < LA_NCH; ch++) {
        char t[32];
        snprintf(t, sizeof t, "D%d  %s", ch, LA.chname[ch]);
        lv_obj_t *row = switch_row(b, t, LA.chshow[ch], ch_show_cb, (void *)(intptr_t)ch);
        lv_obj_t *l = lv_obj_get_child(row, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, open_channel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ch);
    }
    note(b, _("Tocá un nombre para cambiarlo. Los nombres van con la captura cuando se guarda (PulseView los muestra)."));
}

static void meas_sheet(int ch);
static void meas_cb(lv_event_t *e) { meas_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

void la_ui_channel_sheet(int ch)
{
    char t[40];
    snprintf(t, sizeof t, "D%d %s", ch, LA.chname[ch]);
    lv_obj_t *b = sheet(SH_CHANNEL, ch, t);
    lv_obj_t *r = lv_obj_create(b);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    btn(r, AOS_SYM_PENCIL, _("Nombre"), rename_cb, (void *)(intptr_t)ch);
    btn(r, AOS_SYM_RULER, _("Medir"), meas_cb, (void *)(intptr_t)ch);
    switch_row(b, _("Se ve"), LA.chshow[ch], ch_show_cb, (void *)(intptr_t)ch);
    const char *tl[LA_T_COUNT];
    for (int i = 0; i < LA_T_COUNT; i++) tl[i] = _(TRIG_NAME[i]);
    s_trig_ch = ch;
    choices(b, _("Disparo en este canal"), tl, LA_T_COUNT, LA.cfg.trig[ch], trig_set_one);
}

/* ---- decoders ---- */

static void dec_sheet(int slot);
static void decs_sheet(void);

static void dec_on_cb(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    LA.dec[r].on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    U.dec_changed = true;
}

static void dec_open_cb(lv_event_t *e) { dec_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

static void decs_sheet(void)
{
    lv_obj_t *b = sheet(SH_DECS, 0, _("Decodificadores"));
    for (int r = 0; r < LA_DEC_MAX; r++) {
        char t[96];
        la_dec_describe(&LA.dec[r], t, sizeof t);
        lv_obj_t *row = switch_row(b, t, LA.dec[r].on, dec_on_cb, (void *)(intptr_t)r);
        lv_obj_t *l = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, dec_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        if (LA.dec[r].on && LA.an.summary[r][0]) {
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_t *s = note(row, LA.an.summary[r]);
            lv_obj_move_to_index(s, -1);
        }
    }
    note(b, _("Tocá uno para elegir el protocolo, los canales y la velocidad. Se decodifica de nuevo al cerrar."));
}

static int s_slot;
static la_dec_cfg_t *ed(void) { return &LA.dec[s_slot]; }

static void type_set(int i)
{
    la_dec_cfg_t *d = ed();
    int8_t keep = d->ch[0];
    la_dec_defaults(d, i);
    if (keep >= 0 && i != LA_DEC_PAR) d->ch[0] = keep;
    U.dec_changed = true;
    dec_sheet(s_slot);
}

static void pin_set_k(int pin, int i)
{
    ed()->ch[pin] = (int8_t)(i - 1);
    U.dec_changed = true;
}
static void pin_set0(int i) { pin_set_k(0, i); }
static void pin_set1(int i) { pin_set_k(1, i); }
static void pin_set2(int i) { pin_set_k(2, i); }
static void pin_set3(int i) { pin_set_k(3, i); }
static void pin_set4(int i) { pin_set_k(4, i); }
static void pin_set5(int i) { pin_set_k(5, i); }
static void pin_set6(int i) { pin_set_k(6, i); }
static void pin_set7(int i) { pin_set_k(7, i); }
static void pin_set8(int i) { pin_set_k(8, i); }
static const set_fn PIN_SET[LA_DEC_PINS] = { pin_set0, pin_set1, pin_set2, pin_set3, pin_set4, pin_set5, pin_set6, pin_set7, pin_set8 };

static const uint32_t UART_B[] = { 0, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 921600, 1000000 };
static const uint32_t CAN_B[] = { 50000, 100000, 125000, 250000, 500000, 800000, 1000000 };
static void ubaud_set(int i) { ed()->baud = UART_B[i]; U.dec_changed = true; }
static void cbaud_set(int i) { ed()->baud = CAN_B[i]; U.dec_changed = true; }
static void bits_set(int i) { ed()->bits = (uint8_t)(5 + i); U.dec_changed = true; }
static void sbits_set(int i) { static const uint8_t B[] = { 4, 8, 9, 10, 12, 16 }; ed()->bits = B[i]; U.dec_changed = true; }
static void par_set(int i) { ed()->parity = "NEO"[i]; U.dec_changed = true; }
static void stop_set(int i) { ed()->stop = (uint8_t)(i + 1); U.dec_changed = true; }
static void mode_set(int i) { ed()->mode = (uint8_t)i; U.dec_changed = true; }
static void lsb_set(int i) { ed()->lsb = i == 1; U.dec_changed = true; }
static void inv_set(int i) { ed()->inv = i == 1; U.dec_changed = true; }
static void fmt_set(int i) { ed()->fmt = (uint8_t)i; U.dec_changed = true; }

static void dec_onoff_cb(lv_event_t *e)
{
    (void)e;
    ed()->on = !ed()->on;
    U.dec_changed = true;
    dec_sheet(s_slot);
}

static void dec_sheet(int slot)
{
    s_slot = slot;
    la_dec_cfg_t *d = ed();
    char t[48];
    snprintf(t, sizeof t, _("Decodificador %d"), slot + 1);
    bool changed = U.dec_changed;
    lv_obj_t *b = sheet(SH_DEC, slot, t);
    U.dec_changed = changed;
    lv_obj_t *top = btn(b, d->on ? AOS_SYM_CHECK : AOS_SYM_CLOSE, d->on ? _("Encendido") : _("Apagado"), dec_onoff_cb, NULL);
    lv_obj_set_style_bg_color(top, d->on ? AOS_C_GREEN : AOS_C_CARD2, 0);
    const char *tn[LA_DEC_COUNT];
    for (int i = 0; i < LA_DEC_COUNT; i++) tn[i] = la_dec_name(i);
    choices(b, _("Protocolo"), tn, LA_DEC_COUNT, d->type, type_set);
    char cl[LA_NCH + 1][20];
    const char *cp[LA_NCH + 1];
    snprintf(cl[0], sizeof cl[0], "-");
    cp[0] = cl[0];
    for (int i = 0; i < LA_NCH; i++) {
        snprintf(cl[i + 1], sizeof cl[i + 1], "D%d %.8s", i, LA.chname[i]);
        cp[i + 1] = cl[i + 1];
    }
    for (int p = 0; p < la_dec_npins(d->type); p++) {
        char pt[48];
        snprintf(pt, sizeof pt, "%s%s", la_dec_pin_name(d->type, p), la_dec_pin_needed(d->type, p) ? "" : _(" (optativo)"));
        choices(b, pt, cp, LA_NCH + 1, d->ch[p] + 1, PIN_SET[p]);
    }
    if (d->type == LA_DEC_UART || d->type == LA_DEC_MODBUS) {
        char bl[14][12];
        const char *bp[14];
        int cur = 0;
        for (int i = 0; i < 14; i++) {
            if (!i) snprintf(bl[i], sizeof bl[i], "%s", _("Sola"));
            else snprintf(bl[i], sizeof bl[i], "%u", (unsigned)UART_B[i]);
            bp[i] = bl[i];
            if (UART_B[i] == d->baud) cur = i;
        }
        choices(b, _("Velocidad (Sola: la adivina)"), bp, 14, cur, ubaud_set);
        if (d->type == LA_DEC_UART) {
            static const char *const BI[] = { "5", "6", "7", "8", "9" };
            choices(b, _("Bits de datos"), BI, 5, d->bits - 5, bits_set);
        }
        const char *pa[3] = { _("Ninguna"), _("Par"), _("Impar") };
        choices(b, _("Paridad"), pa, 3, d->parity == 'E' ? 1 : d->parity == 'O' ? 2 : 0, par_set);
        static const char *const ST[] = { "1", "2" };
        choices(b, _("Bits de parada"), ST, 2, d->stop == 2 ? 1 : 0, stop_set);
        const char *iv[2] = { _("Normal"), _("Invertida") };
        choices(b, _("Línea"), iv, 2, d->inv, inv_set);
    } else if (d->type == LA_DEC_CAN) {
        static const char *const CB[] = { "50k", "100k", "125k", "250k", "500k", "800k", "1M" };
        int cur = 4;
        for (int i = 0; i < 7; i++)
            if (CAN_B[i] == d->baud) cur = i;
        choices(b, _("Velocidad del bus"), CB, 7, cur, cbaud_set);
    } else if (d->type == LA_DEC_SPI) {
        static const char *const MO[] = { "0", "1", "2", "3" };
        choices(b, _("Modo (CPOL y CPHA)"), MO, 4, d->mode, mode_set);
        static const char *const SB[] = { "4", "8", "9", "10", "12", "16" };
        static const uint8_t SV[] = { 4, 8, 9, 10, 12, 16 };
        int cur = 1;
        for (int i = 0; i < 6; i++)
            if (SV[i] == d->bits) cur = i;
        choices(b, _("Bits por palabra"), SB, 6, cur, sbits_set);
        const char *ord[2] = { _("MSB primero"), _("LSB primero") };
        choices(b, _("Orden"), ord, 2, d->lsb, lsb_set);
        const char *cs[2] = { _("Activo en 0"), _("Activo en 1") };
        choices(b, "CS", cs, 2, d->inv, inv_set);
    } else if (d->type == LA_DEC_NEC) {
        const char *iv[2] = { _("0 (receptor)"), _("1 (LED)") };
        choices(b, _("Nivel con portadora"), iv, 2, d->inv, inv_set);
    }
    if (d->type != LA_DEC_WS2812 && d->type != LA_DEC_NEC && d->type != LA_DEC_MODBUS) {
        const char *fm[LA_FMT_COUNT] = { _("Hexa"), _("Decimal"), "ASCII", _("Binario") };
        choices(b, _("Valores en"), fm, LA_FMT_COUNT, d->fmt, fmt_set);
    }
    if (d->type == LA_DEC_CAN)
        note(b, _("Medí del lado del transceptor (RX del controlador), nunca CANH/CANL directo: 3,3 V como mucho."));
    if (d->type == LA_DEC_MODBUS || d->type == LA_DEC_DMX)
        note(b, _("En RS485 medí la salida RO del transceptor, no las líneas A/B."));
}

void la_ui_decoder_sheet(int slot)
{
    if (slot >= 0 && slot < LA_DEC_MAX) dec_sheet(slot);
}

/* ---- the table ---- */

static void table_sheet(void);
#define TAB_PAGE 60

static void tab_go_cb(lv_event_t *e)
{
    uint32_t i = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (!LA.an_ok || i >= LA.an.npkt) return;
    uint32_t a = LA.an.pkt[i].s0, z = LA.an.pkt[i].s1;
    la_sheet_close();
    la_view_goto(a, z);
}

static void tab_page_cb(lv_event_t *e)
{
    U.tab_page += (int)(intptr_t)lv_event_get_user_data(e);
    if (U.tab_page < 0) U.tab_page = 0;
    table_sheet();
}

static int s_filter_rows[LA_DEC_MAX + 1];
static void filter_set(int i)
{
    U.tab_filter = s_filter_rows[i];
    U.tab_page = 0;
    table_sheet();
}

static void table_sheet(void)
{
    lv_obj_t *b = sheet(SH_TABLE, 0, _("Tabla"));
    if (!LA.an_ok || !LA.an.npkt) {
        note(b, LA.busy ? _("Decodificando...") : _("No hay paquetes: prendé un decodificador (Decodificar) y capturá."));
        return;
    }
    const char *fl[LA_DEC_MAX + 1];
    char fb[LA_DEC_MAX][24];
    int nf = 0, fcur = 0;
    fl[nf] = _("Todos");
    s_filter_rows[nf++] = -1;
    for (int r = 0; r < LA_DEC_MAX; r++)
        if (LA.dec[r].on && LA.an.row_n[r]) {
            snprintf(fb[r], sizeof fb[r], "%d %s", r + 1, la_dec_name(LA.dec[r].type));
            if (U.tab_filter == r) fcur = nf;
            fl[nf] = fb[r];
            s_filter_rows[nf++] = r;
        }
    if (!fcur) U.tab_filter = -1;
    if (nf > 2) choices(b, NULL, fl, nf, fcur, filter_set);
    /* the rows of this page */
    uint32_t match = 0, shown = 0, first = (uint32_t)U.tab_page * TAB_PAGE;
    const float r = LA.cap.rate ? (float)LA.cap.rate : 1;
    const int32_t t0 = LA.cap.trig >= 0 ? LA.cap.trig : 0;
    for (uint32_t i = 0; i < LA.an.npkt; i++) {
        const la_pkt_t *p = &LA.an.pkt[i];
        if (U.tab_filter >= 0 && p->row != U.tab_filter) continue;
        if (match++ < first || shown >= TAB_PAGE) continue;
        shown++;
        lv_obj_t *c = card(b);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 2, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, tab_go_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        char ts[24], h[64];
        la_fmt_time(((float)p->s0 - (float)t0) / r, ts, sizeof ts);
        snprintf(h, sizeof h, "%s · %d %s", ts, p->row + 1, la_dec_name(LA.dec[p->row].type));
        aos_make_decorative(aos_label(c, h, aos_font_caption, AOS_C_DIM));
        lv_obj_t *l = aos_label(c, p->t, &aos_mono_18, p->err ? AOS_C_RED : AOS_C_TEXT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        aos_make_decorative(l);
    }
    lv_obj_t *row = lv_obj_create(b);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (U.tab_page > 0) btn(row, AOS_SYM_CHEVRON_LEFT, _("Anteriores"), tab_page_cb, (void *)(intptr_t)-1);
    char pg[64];
    snprintf(pg, sizeof pg, _("%u a %u de %u"), (unsigned)(match ? first + 1 : 0), (unsigned)(first + shown), (unsigned)match);
    aos_label(row, pg, aos_font_small, AOS_C_DIM);
    if (first + shown < match) btn(row, AOS_SYM_CHEVRON_RIGHT, _("Siguientes"), tab_page_cb, (void *)(intptr_t)1);
    if (LA.an.full) note(b, _("Hubo más de lo que entra: el final de la captura no se decodificó."));
}

void la_ui_packet_toast(uint32_t s, int row)
{
    for (uint32_t i = 0; i < LA.an.npkt; i++) {
        const la_pkt_t *p = &LA.an.pkt[i];
        if (p->row == row && p->s0 <= s && s <= p->s1) {
            aos_ui_toast(p->t, 4000);
            return;
        }
    }
}

/* ---- measuring ---- */

static void meas_fill(void)
{
    if (!U.meas_lbl) return;
    if (!LA.cap.n || !LA.cap_ok) {
        lv_label_set_text(U.meas_lbl, _("No hay captura"));
        return;
    }
    uint32_t a, z;
    bool cur = la_view_cursors(&a, &z);
    if (!cur) la_view_visible(&a, &z);
    la_meas_t m;
    la_measure(&LA.cap, U.meas_ch, a, z, &m);
    char b[600], t1[24], t2[24], t3[24], t4[24];
    int k = snprintf(b, sizeof b, "%s\n", cur ? _("Entre los cursores A y B") : _("Lo que se ve en pantalla"));
    la_fmt_time((float)(z - a) / (float)LA.cap.rate, t1, sizeof t1);
    k += snprintf(b + k, sizeof b - k, _("Tramo: %s, %u muestras\n"), t1, (unsigned)(z - a));
    k += snprintf(b + k, sizeof b - k, _("Flancos: %u de subida, %u de bajada\n"), (unsigned)m.rises, (unsigned)m.falls);
    if (m.freq > 0) {
        la_fmt_freq(m.freq, t1, sizeof t1);
        la_fmt_time(1.0f / m.freq, t2, sizeof t2);
        k += snprintf(b + k, sizeof b - k, _("Frecuencia: %s (período %s)\n"), t1, t2);
        k += snprintf(b + k, sizeof b - k, _("Ciclo de trabajo: %.1f %%\n"), (double)(m.duty * 100));
    }
    if (m.hi_max > 0) {
        la_fmt_time(m.hi_min, t1, sizeof t1);
        la_fmt_time(m.hi_avg, t2, sizeof t2);
        la_fmt_time(m.hi_max, t3, sizeof t3);
        k += snprintf(b + k, sizeof b - k, _("En 1: mín %s, prom %s, máx %s\n"), t1, t2, t3);
    }
    if (m.lo_max > 0) {
        la_fmt_time(m.lo_min, t1, sizeof t1);
        la_fmt_time(m.lo_avg, t2, sizeof t2);
        la_fmt_time(m.lo_max, t4, sizeof t4);
        k += snprintf(b + k, sizeof b - k, _("En 0: mín %s, prom %s, máx %s\n"), t1, t2, t4);
    }
    snprintf(b + k, sizeof b - k, _("En 1 el %.1f %% del tramo"), (double)(m.high_frac * 100));
    lv_label_set_text(U.meas_lbl, b);
}

static void meas_ch_set(int i)
{
    U.meas_ch = i;
    meas_fill();
}

static void meas_sheet(int ch)
{
    if (ch >= 0) U.meas_ch = ch;
    lv_obj_t *b = sheet(SH_MEAS, 0, _("Medir"));
    char cl[LA_NCH][20];
    const char *cp[LA_NCH];
    for (int i = 0; i < LA_NCH; i++) {
        snprintf(cl[i], sizeof cl[i], "D%d %.8s", i, LA.chname[i]);
        cp[i] = cl[i];
    }
    choices(b, _("Canal"), cp, LA_NCH, U.meas_ch, meas_ch_set);
    lv_obj_t *c = card(b);
    U.meas_lbl = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(U.meas_lbl, LV_PCT(100));
    lv_label_set_long_mode(U.meas_lbl, LV_LABEL_LONG_MODE_WRAP);
    note(b, _("Con los dos cursores puestos se mide entre ellos (tocá la señal para ponerlos, mantené apretado para "
              "sacarlos); si no, lo que está en pantalla."));
    meas_fill();
}

/* ---- files ---- */

static void files_sheet(void);

static void save_cb(lv_event_t *e)
{
    (void)e;
    const char *d = la_dir();
    if (!d) {
        aos_ui_toast(_("No hay tarjeta"), 2000);
        return;
    }
    char p[160];
    if (aos_hal_time_is_valid()) {
        struct tm tm;
        aos_hal_time_now(&tm);
        snprintf(p, sizeof p, "%s/cap_%04d%02d%02d_%02d%02d%02d.sr", d, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        for (int i = 1; i < 10000; i++) {
            snprintf(p, sizeof p, "%s/cap_%04d.sr", d, i);
            struct stat st;
            if (stat(p, &st) != 0) break;
        }
    }
    if (la_save_start(p)) {
        la_set_msg(false, "%s", _("Guardando..."));
        la_sheet_close();
    }
}

static void open_cb(lv_event_t *e)
{
    const char *name = lv_label_get_text(lv_obj_get_child(lv_event_get_current_target(e), 0));
    char p[200];
    snprintf(p, sizeof p, "%s/%s", la_dir(), name);
    if (la_load_start(p)) la_sheet_close();
}

static void del_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target(e));
    const char *name = lv_label_get_text(lv_obj_get_child(lv_obj_get_child(row, 0), 0));
    if (strcmp(U.del_name, name) || la_ms() - U.del_at > 4000) {
        snprintf(U.del_name, sizeof U.del_name, "%s", name);
        U.del_at = la_ms();
        aos_ui_toast(_("Tocá otra vez para borrarla"), 2000);
        return;
    }
    char p[200];
    snprintf(p, sizeof p, "%s/%s", la_dir(), name);
    unlink(p);
    char *dot = strrchr(p, '.');
    if (dot) {
        snprintf(dot, sizeof p - (size_t)(dot - p), ".csv");
        unlink(p);
    }
    U.del_name[0] = 0;
    files_sheet();
}

static int name_cmp(const void *a, const void *b) { return -strcmp((const char *)a, (const char *)b); }

static void files_sheet(void)
{
    lv_obj_t *b = sheet(SH_FILES, 0, _("Archivos"));
    const char *d = la_dir();
    if (!d) {
        note(b, _("No hay tarjeta."));
        return;
    }
    lv_obj_t *sv = btn(b, AOS_SYM_DOWNLOAD, _("Guardar la captura"), save_cb, NULL);
    if (!LA.cap.n) lv_obj_add_state(sv, LV_STATE_DISABLED);
    char (*names)[64] = la_big_alloc(200 * 64);
    int n = 0;
    DIR *dir = names ? opendir(d) : NULL;
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) && n < 200) {
            size_t l = strlen(de->d_name);
            if (de->d_name[0] == '.' || l < 4 || l > 63 || strcmp(de->d_name + l - 3, ".sr")) continue;
            memcpy(names[n++], de->d_name, l + 1);
        }
        closedir(dir);
    }
    if (n > 1) qsort(names, (size_t)n, 64, name_cmp);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(b);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 10, 0);
        lv_obj_t *c = card(row);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_width(c, 10);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, open_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = aos_label(c, names[i], aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, LV_PCT(100));
        aos_make_decorative(l);
        char p[200];
        snprintf(p, sizeof p, "%s/%s", d, names[i]);
        struct stat st;
        if (stat(p, &st) == 0) {
            char s[32];
            snprintf(s, sizeof s, "%u KB", (unsigned)((st.st_size + 1023) / 1024));
            aos_make_decorative(aos_label(c, s, aos_font_caption, AOS_C_DIM));
        }
        lv_obj_t *x = btn(row, AOS_SYM_DELETE, NULL, del_cb, NULL);
        lv_obj_set_style_bg_color(x, lv_color_hex(0x3A1C1C), 0);
    }
    la_big_free(names);
    if (!n) note(b, _("Todavía no hay capturas guardadas."));
    note(b, _("Se guardan como sesiones de sigrok (.sr): PulseView las abre, y la placa abre las de PulseView. Con un "
              "decodificador encendido va también la tabla en .csv. Están en la carpeta logica de la tarjeta y en la "
              "página Lógica del portal."));
}

/* ---- the bar ---- */

static void dev_cb(lv_event_t *e) { (void)e; dev_sheet(); }
static void sampling_cb(lv_event_t *e) { (void)e; sampling_sheet(); }
static void trig_cb(lv_event_t *e) { (void)e; trig_sheet(); }
static void channels_cb(lv_event_t *e) { (void)e; channels_sheet(); }
static void decs_cb(lv_event_t *e) { (void)e; decs_sheet(); }
static void table_cb(lv_event_t *e) { (void)e; U.tab_page = 0; table_sheet(); }
static void meas_bar_cb(lv_event_t *e) { (void)e; meas_sheet(-1); }
static void files_cb(lv_event_t *e) { (void)e; files_sheet(); }
static void zoom_in_cb(lv_event_t *e) { (void)e; la_view_zoom(2); }
static void zoom_out_cb(lv_event_t *e) { (void)e; la_view_zoom(0.5f); }
static void fit_cb(lv_event_t *e) { (void)e; la_view_fit(); }
static void to_trig_cb(lv_event_t *e) { (void)e; la_view_to_trigger(); }

static void rep_cb(lv_event_t *e)
{
    (void)e;
    LA.cfg.repeat = !LA.cfg.repeat;
    if (!LA.cfg.repeat) la_capture_cancel_repeat();
    la_cfg_save();
    la_ui_status();
}

static void cap_cb(lv_event_t *e)
{
    (void)e;
    if (LA.busy) {
        la_capture_cancel_repeat();
        la_job_stop();
    } else {
        la_cfg_save();
        if (!la_capture_ui() && !LA.msg[0]) la_set_msg(true, "%s", _("No se pudo arrancar la captura"));
    }
    la_ui_status();
}

static void trig_text(char *out, size_t n)
{
    out[0] = 0;
    int k = 0;
    static const char *const S[LA_T_COUNT] = { "", "=0", "=1", N_("sube"), N_("baja"), N_("cambia") };
    for (int ch = 0; ch < LA_NCH; ch++) {
        if (LA.cfg.trig[ch] == LA_T_X) continue;
        if (k++ == 2) {
            snprintf(out + strlen(out), n - strlen(out), " ...");
            break;
        }
        snprintf(out + strlen(out), n - strlen(out), "%sD%d %s", k > 1 ? ", " : "", ch,
                 LA.cfg.trig[ch] <= LA_T_1 ? S[LA.cfg.trig[ch]] : _(S[LA.cfg.trig[ch]]));
    }
    if (!k) snprintf(out, n, "%s", _("Sin disparo"));
}

void la_ui_status(void)
{
    if (!U.status) return;
    char b[200], r[16], s[16];
    const la_dev_t *d = LA.ndevs ? &LA.devs[LA.dev] : NULL;
    if (d) {
        lv_label_set_text(U.dev_name, d->name);
        lv_label_set_text(U.dev_detail, d->detail);
        lv_obj_set_style_bg_color(U.dev_dot, d->drv == LA_DRV_DEMO ? AOS_C_DIM : AOS_C_GREEN, 0);
    }
    la_fmt_rate(LA.cfg.rate, r, sizeof r);
    lv_label_set_text(U.rate_lbl, r);
    la_fmt_count(LA.cfg.samples, s, sizeof s);
    lv_label_set_text(U.samp_lbl, s);
    trig_text(b, sizeof b);
    lv_label_set_text(U.trig_lbl, b);
    if (LA.cfg.repeat) lv_obj_add_state(U.rep_btn, LV_STATE_CHECKED);
    else lv_obj_remove_state(U.rep_btn, LV_STATE_CHECKED);

    la_lock();
    bool busy = LA.busy, err = LA.msg_err;
    int st = LA.state;
    uint32_t prog = LA.prog, tot = LA.prog_total;
    char msg[96];
    snprintf(msg, sizeof msg, "%s", LA.msg);
    la_unlock();
    lv_label_set_text(U.cap_lbl, busy ? _("Detener") : _("Capturar"));
    lv_label_set_text(U.cap_ico, busy ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_obj_set_style_bg_color(U.cap_btn, busy ? AOS_C_RED : AOS_C_GREEN, 0);

    lv_color_t col = AOS_C_DIM;
    if (busy) {
        int pct = tot ? (int)((uint64_t)prog * 100 / tot) : 0;
        if (pct > 100) pct = 100;
        switch (st) {
        case LA_ST_CONNECT: snprintf(b, sizeof b, "%s", _("Conectando con el analizador...")); break;
        case LA_ST_WAIT: snprintf(b, sizeof b, _("Esperando el disparo (%d %% del búfer lleno)..."), pct); break;
        case LA_ST_CAPTURE: snprintf(b, sizeof b, _("Capturando: %d %%"), pct); break;
        case LA_ST_DECODE: snprintf(b, sizeof b, "%s", _("Decodificando...")); break;
        default: snprintf(b, sizeof b, _("Archivo: %d %%"), pct); break;
        }
        col = AOS_C_ORANGE;
    } else {
        snprintf(b, sizeof b, "%s", msg);
        col = err ? AOS_C_RED : AOS_C_DIM;
    }
    lv_label_set_text(U.status, b);
    lv_obj_set_style_text_color(U.status, col, 0);

    /* the capture and the cursors */
    if (LA.cap_ok && LA.cap.n) {
        char n[16], rr[16], dur[24];
        la_fmt_count(LA.cap.n, n, sizeof n);
        la_fmt_rate(LA.cap.rate, rr, sizeof rr);
        la_fmt_time((float)LA.cap.n / (float)LA.cap.rate, dur, sizeof dur);
        int k = snprintf(b, sizeof b, "%.30s · %s @ %s · %s", LA.cap.src, n, rr, dur);
        uint32_t a, z;
        if (la_view_cursors(&a, &z)) {
            char dt[24], f[24];
            float sec = (float)(z - a) / (float)LA.cap.rate;
            la_fmt_time(sec, dt, sizeof dt);
            if (sec > 0) la_fmt_freq(1.0f / sec, f, sizeof f);
            else snprintf(f, sizeof f, "-");
            snprintf(b + k, sizeof b - k, _(" · A-B %s (%s)"), dt, f);
        }
    } else {
        snprintf(b, sizeof b, "%s", "");
    }
    lv_label_set_text(U.info, b);
}

void la_ui_after_job(void)
{
    if (U.sheet_kind == SH_FILES) files_sheet();
    else if (U.sheet_kind == SH_DEV) dev_sheet();
    else if (U.sheet_kind == SH_TABLE) table_sheet();
    else if (U.sheet_kind == SH_MEAS) meas_fill();
    la_ui_status();
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = btn(parent, glyph, U.land ? text : NULL, cb, NULL);
    if (!U.land) {
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_pad_hor(b, 4, 0);
    }
    return b;
}

void la_ui_build(lv_obj_t *root)
{
    U.root = root;
    U.sheet = NULL;
    U.sheet_kind = SH_NONE;
    lv_obj_clean(root);
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    const int pad = 16;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, U.W - 2 * pad, LV_SIZE_CONTENT);
    lv_obj_set_pos(bar, pad, 8);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* the analyser */
    lv_obj_t *dev = lv_obj_create(bar);
    lv_obj_remove_style_all(dev);
    lv_obj_set_height(dev, 72);
    lv_obj_set_style_radius(dev, 20, 0);
    lv_obj_set_style_bg_color(dev, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(dev, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(dev, 16, 0);
    lv_obj_add_flag(dev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dev, dev_cb, LV_EVENT_CLICKED, NULL);
    if (U.land) lv_obj_set_flex_grow(dev, 1);
    else lv_obj_set_width(dev, LV_PCT(100));
    U.dev_dot = lv_obj_create(dev);
    lv_obj_remove_style_all(U.dev_dot);
    lv_obj_set_size(U.dev_dot, 16, 16);
    lv_obj_set_style_radius(U.dev_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.dev_dot, LV_OPA_COVER, 0);
    lv_obj_align(U.dev_dot, LV_ALIGN_LEFT_MID, 0, 0);
    U.dev_name = aos_label(dev, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(U.dev_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(U.dev_name, LV_PCT(92));
    lv_obj_align(U.dev_name, LV_ALIGN_TOP_LEFT, 28, 8);
    U.dev_detail = aos_label(dev, "", aos_font_tiny, AOS_C_DIM);
    lv_label_set_long_mode(U.dev_detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(U.dev_detail, LV_PCT(92));
    lv_obj_align(U.dev_detail, LV_ALIGN_BOTTOM_LEFT, 28, -10);
    aos_make_decorative(U.dev_dot);
    aos_make_decorative(U.dev_name);
    aos_make_decorative(U.dev_detail);

    lv_obj_t *rb = btn(bar, AOS_SYM_SPEEDOMETER, "", sampling_cb, NULL);
    U.rate_lbl = lv_obj_get_child(rb, 1);
    lv_obj_t *sb = btn(bar, AOS_SYM_MEMORY, "", sampling_cb, NULL);
    U.samp_lbl = lv_obj_get_child(sb, 1);
    lv_obj_t *tb = btn(bar, AOS_SYM_FLAG_OUTLINE, "", trig_cb, NULL);
    U.trig_lbl = lv_obj_get_child(tb, 1);
    lv_label_set_long_mode(U.trig_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(U.trig_lbl, U.land ? 200 : 130, 0);
    lv_obj_set_height(U.trig_lbl, lv_font_get_line_height(aos_font_small));   /* one line, cut */
    U.rep_btn = btn(bar, AOS_SYM_RESTART, NULL, rep_cb, NULL);
    U.cap_btn = btn(bar, AOS_SYM_PLAY, _("Capturar"), cap_cb, NULL);
    U.cap_ico = lv_obj_get_child(U.cap_btn, 0);
    U.cap_lbl = lv_obj_get_child(U.cap_btn, 1);
    if (U.land) lv_obj_set_style_min_width(U.cap_btn, 190, 0);
    else lv_obj_set_flex_grow(U.cap_btn, 1);
    lv_obj_update_layout(root);
    int32_t y = 8 + lv_obj_get_height(bar) + 6;

    int32_t lh = lv_font_get_line_height(aos_font_caption);
    U.status = aos_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_size(U.status, U.W - 2 * pad, lh);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.status, pad, y);
    y += lh + 2;
    U.info = aos_label(root, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_size(U.info, U.W - 2 * pad, lh);
    lv_label_set_long_mode(U.info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.info, pad, y);
    y += lh + 6;

    /* the tools, at the bottom */
    lv_obj_t *tools = lv_obj_create(root);
    lv_obj_remove_style_all(tools);
    lv_obj_set_size(tools, U.W - 2 * pad, 72);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(tools, U.land ? 10 : 6, 0);
    lv_obj_remove_flag(tools, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tools, LV_ALIGN_BOTTOM_MID, 0, -10);
    small_btn(tools, AOS_SYM_WAVEFORM, _("Canales"), channels_cb);
    small_btn(tools, AOS_SYM_TRANSLATE, _("Decodificar"), decs_cb);
    small_btn(tools, AOS_SYM_FILE_DELIMITED_OUTLINE, _("Tabla"), table_cb);
    small_btn(tools, AOS_SYM_RULER, _("Medir"), meas_bar_cb);
    small_btn(tools, AOS_SYM_FOLDER, _("Archivos"), files_cb);
    lv_obj_t *sp = lv_obj_create(tools);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, 1);
    if (U.land) lv_obj_set_flex_grow(sp, 1);
    small_btn(tools, AOS_SYM_MINUS, NULL, zoom_out_cb);
    small_btn(tools, AOS_SYM_PLUS, NULL, zoom_in_cb);
    small_btn(tools, AOS_SYM_ARROW_EXPAND_HORIZONTAL, NULL, fit_cb);
    small_btn(tools, AOS_SYM_FLAG_OUTLINE, NULL, to_trig_cb);

    int32_t vh = U.H - y - 72 - 20;
    lv_obj_t *holder = lv_obj_create(root);
    lv_obj_remove_style_all(holder);
    lv_obj_set_pos(holder, 0, y);
    lv_obj_set_size(holder, U.W, vh);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
    la_view_create(holder, U.W, vh);
    la_view_reset();
    la_ui_status();
}
