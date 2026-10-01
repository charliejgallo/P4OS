/*
 * P4OS - Banco: the bench's instruments on one screen.
 *
 * A strip of four live tiles - a row on top in portrait, a column on the
 * left in landscape - that are at once the tabs and a dashboard: each says
 * what its instrument is doing right now, whichever tab is open.
 *
 *   Osciloscopio  the Rigol DS1000Z over SCPI (aos_scope.c, aos_bench_scope.c)
 *   Fuente        the Riden RD60xx over Modbus RTU (aos_riden.c): the output
 *                 big, setpoints read back, a trace of V and A
 *   Generador     the UNI-T UTG932E: USBTMC over USB, which waits for a
 *                 powered hub (the P4's OTG port gives no 5 V)
 *   Registro      a CSV on the card with the supply's readings and the
 *                 scope's measurements, at a fixed pace (aos_bench_log.c)
 */
#include "aos_apps.h"
#include "aos_bench.h"
#include "aos_bench_log.h"
#include "aos_riden.h"
#include "aos_scope.h"
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

#define C_BENCH   lv_color_hex(0xEF4444)
#define C_AMBER   lv_color_hex(0xF59E0B)

enum { TAB_SCOPE, TAB_PSU, TAB_GEN, TAB_LOG, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { N_("Osciloscopio"), N_("Fuente"), N_("Generador"), N_("Registro") };

#define PSU_HIST 240                /* two minutes at two a second */

/* Kept across rotations and tab changes. */
static struct {
    int tab;
    bool loaded;
    float v_hist[PSU_HIST], i_hist[PSU_HIST];
    int h_n;
    uint32_t rd_seen_t;
    int log_focus;                  /* the series the Registro's chart shows */
} S;
/* Not initialised in place, so that it is .bss and goes to PSRAM (psram.lf). */
__attribute__((constructor)) static void S_defaults(void) { S.log_focus = -1; }

static struct {
    lv_obj_t *root, *content, *tile[TAB_COUNT], *tile_val[TAB_COUNT], *tile_sub[TAB_COUNT], *overlay;
    lv_timer_t *timer;
    int32_t W, H, cw, ch;
    bool land;
    uint32_t ticks, rd_seq, rd_wseq, log_seq;
    /* Fuente */
    lv_obj_t *p_v, *p_mode, *p_i, *p_p, *p_out, *p_out_l, *p_vset, *p_iset, *p_cap, *p_fact[5], *p_chart, *p_model, *p_err;
    lv_chart_series_t *p_sv, *p_si;
    /* Registro */
    lv_obj_t *l_time, *l_rows, *l_path, *l_btn, *l_btn_l, *l_chart, *l_legend, *l_focus;
    lv_chart_series_t *l_ser;
    /* the keypad */
    char kp_text[16];
    lv_obj_t *kp_value;
    void (*kp_done)(double v);
    int kp_dec;
    bool kp_fresh;
} U;

static void build_tab(void);

/* -------------------------------------------------------------------------- */
/* Small pieces                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_color_t on_c, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? on_c : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    return c;
}

static lv_obj_t *chips_row(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

/* 12.34 -> "12,34" */
static void fmt_num(char *out, size_t n, double v, int dec)
{
    snprintf(out, n, "%.*f", dec, v);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

static void fmt_unit(char *out, size_t n, double v, int dec, const char *unit)
{
    char t[24];
    fmt_num(t, sizeof t, v, dec);
    snprintf(out, n, "%.15s %.6s", t, unit);
}

/* 1234.5 Hz -> "1,235 kHz", 0.0123 V -> "12,3 mV" */
static void fmt_eng(char *out, size_t n, double v, const char *unit)
{
    static const struct { double f; const char *p; } P[] = { { 1e6, "M" }, { 1e3, "k" }, { 1, "" }, { 1e-3, "m" }, { 1e-6, "µ" } };
    double a = fabs(v);
    int k = 2;
    for (int i = 0; i < 5; i++) if (a >= P[i].f * 0.9995) { k = i; break; }
    if (a < 1e-6) k = 2;
    if (strcmp(unit, "Hz") && k < 2) k = 2;         /* no kV, no MA on a bench */
    double s = v / P[k].f, as = fabs(s);
    char t[24];
    fmt_num(t, sizeof t, s, as >= 100 ? 1 : as >= 10 ? 2 : 3);
    snprintf(out, n, "%s %s%s", t, P[k].p, unit);
}

/* ---- the numeric keypad ---- */

static void kp_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.kp_value = NULL;
}

static void kp_show(void) { if (U.kp_value) lv_label_set_text(U.kp_value, U.kp_text[0] ? U.kp_text : "0"); }

static void kp_key_cb(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(U.kp_text);
    if (!strcmp(k, "ok")) {
        char t[16];
        snprintf(t, sizeof t, "%s", U.kp_text);
        for (char *p = t; *p; p++) if (*p == ',') *p = '.';
        double v = strtod(t, NULL);
        void (*done)(double) = U.kp_done;
        kp_close();
        if (done) done(v);
        return;
    }
    if (!strcmp(k, "x")) { kp_close(); return; }
    if (U.kp_fresh && strcmp(k, "<")) { U.kp_text[0] = 0; n = 0; }
    U.kp_fresh = false;
    if (!strcmp(k, "<")) { if (n) U.kp_text[n - 1] = 0; }
    else if (!strcmp(k, ",")) { if (U.kp_dec && !strchr(U.kp_text, ',') && n < 10) strcat(U.kp_text, n ? "," : "0,"); }
    else if (n < 10) {
        const char *c = strchr(U.kp_text, ',');
        if (!c || (int)strlen(c + 1) < U.kp_dec) strcat(U.kp_text, k);
    }
    kp_show();
}

static void keypad(const char *title, double value, int dec, const char *unit, void (*done)(double))
{
    kp_close();
    U.kp_done = done;
    U.kp_dec = dec;
    U.kp_fresh = true;
    fmt_num(U.kp_text, sizeof U.kp_text, value, dec);
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t kw = U.land ? 150 : 180, kh = U.land ? 82 : 104, gap = 12;
    lv_obj_t *sh = card(U.overlay, 3 * kw + 4 * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 22, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *vr = box(sh, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vr, 10, 0);
    U.kp_value = aos_label(vr, "", aos_font_huge, C_AMBER);
    if (unit && unit[0]) aos_label(vr, unit, aos_font_title, AOS_C_DIM);
    kp_show();
    static const char *const K[15] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", ",", "0", "<", "x", "ok", NULL };
    for (int i = 0; K[i]; i++) {
        bool wide = !strcmp(K[i], "ok");
        lv_obj_t *b = box(sh, wide ? 2 * kw + gap : kw, kh);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, wide ? C_AMBER : i >= 9 ? AOS_C_CARD2 : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_bg_opa(b, (!U.kp_dec && !strcmp(K[i], ",")) ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, kp_key_cb, LV_EVENT_CLICKED, (void *)K[i]);
        const char *txt = !strcmp(K[i], "<") ? LV_SYMBOL_BACKSPACE : !strcmp(K[i], "x") ? _("Cancelar") : !strcmp(K[i], "ok") ? _("Aplicar") : K[i];
        lv_obj_center(aos_label(b, txt, i < 12 && strcmp(K[i], "<") ? aos_font_title : aos_font_body, AOS_C_TEXT));
    }
}

/* -------------------------------------------------------------------------- */
/* The tiles                                                                   */
/* -------------------------------------------------------------------------- */

static void tiles_refresh(void)
{
    char v[48], s[64];
    /* scope */
    aos_scope_status_t sc;
    aos_scope_status(&sc);
    if (sc.connected) {
        snprintf(v, sizeof v, "%s", sc.trig_status[0] ? sc.trig_status : "--");
        const char *model = strchr(sc.idn, ',');
        char m[24] = "Rigol";
        if (model) { snprintf(m, sizeof m, "%.23s", model + 1); char *c = strchr(m, ','); if (c) *c = 0; }
        if (sc.fps > 0.05f) snprintf(s, sizeof s, "%s · %.0f/s", m, (double)sc.fps);
        else snprintf(s, sizeof s, "%s", m);
    } else {
        snprintf(v, sizeof v, "--");
        snprintf(s, sizeof s, "%s", sc.connecting ? _("conectando…") : sc.host[0] ? _("sin conexión") : _("sin configurar"));
    }
    lv_label_set_text(U.tile_val[TAB_SCOPE], v);
    lv_obj_set_style_text_color(U.tile_val[TAB_SCOPE], sc.connected ? AOS_C_YELLOW : AOS_C_DIM, 0);
    lv_label_set_text(U.tile_sub[TAB_SCOPE], s);
    /* supply */
    aos_riden_status_t r;
    aos_riden_status(&r);
    if (r.ok) {
        fmt_unit(v, sizeof v, r.v_out, 2, "V");
        char a[24];
        fmt_unit(a, sizeof a, r.i_out, r.idec, "A");
        snprintf(s, sizeof s, "%s · %s", a, !r.on ? _("apagada") : r.protect ? (r.protect == 1 ? "OVP" : "OCP") : r.cc ? "CC" : "CV");
    } else {
        snprintf(v, sizeof v, "--,-- V");
        snprintf(s, sizeof s, "%s", r.linked ? _("buscando…") : _("sin enlace"));
    }
    lv_label_set_text(U.tile_val[TAB_PSU], v);
    lv_obj_set_style_text_color(U.tile_val[TAB_PSU], !r.ok ? AOS_C_DIM : !r.on ? AOS_C_TEXT : r.cc ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    lv_label_set_text(U.tile_sub[TAB_PSU], s);
    /* generator: nothing to ask it yet */
    lv_label_set_text(U.tile_val[TAB_GEN], "UTG932E");
    lv_label_set_text(U.tile_sub[TAB_GEN], _("falta el USB"));
    /* logger */
    bench_log_stat_t l;
    bench_log_stat(&l);
    if (l.running) {
        uint32_t e = ((uint32_t)aos_hal_uptime_ms() - l.started_ms) / 1000;
        if (e < 3600) snprintf(v, sizeof v, "%02u:%02u", (unsigned)(e / 60), (unsigned)(e % 60));
        else snprintf(v, sizeof v, "%u:%02u:%02u", (unsigned)(e / 3600), (unsigned)(e / 60 % 60), (unsigned)(e % 60));
        snprintf(s, sizeof s, _("%u filas"), (unsigned)l.rows);
    } else {
        snprintf(v, sizeof v, "%s", _("detenido"));
        snprintf(s, sizeof s, "%s", _("CSV a la tarjeta"));
    }
    lv_label_set_text(U.tile_val[TAB_LOG], v);
    lv_obj_set_style_text_color(U.tile_val[TAB_LOG], l.running ? AOS_C_RED : AOS_C_DIM, 0);
    lv_label_set_text(U.tile_sub[TAB_LOG], s);
}

static void tile_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == S.tab) return;
    S.tab = t;
    aos_hal_pref_set_i32("bench_tab", t);
    build_tab();
}

static void build_tiles(lv_obj_t *strip, int32_t tw, int32_t th)
{
    static const char *const GLYPH[TAB_COUNT] = { AOS_SYM_SINE_WAVE, AOS_SYM_LIGHTNING_BOLT, AOS_SYM_WAVEFORM, AOS_SYM_RECORD_REC };
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = card(strip, tw, th);
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_pad_all(t, 14, 0);
        lv_obj_set_style_border_color(t, C_BENCH, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *head = box(t, lv_pct(100), 32);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 8, 0);
        if (U.land) aos_make_decorative(aos_label(head, GLYPH[i], &aos_sym_28, AOS_C_DIM));   /* no room beside the name in portrait */
        lv_obj_t *n = aos_label(head, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM);
        lv_obj_set_height(n, lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(n, 1);
        const lv_font_t *vf = U.land ? aos_font_body : aos_font_small;     /* a quarter of 720 is narrow */
        U.tile_val[i] = aos_label(t, "", vf, AOS_C_TEXT);
        lv_obj_set_size(U.tile_val[i], lv_pct(100), lv_font_get_line_height(vf));
        lv_label_set_long_mode(U.tile_val[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(U.tile_val[i], LV_ALIGN_LEFT_MID, 0, 6);
        U.tile_sub[i] = aos_label(t, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_size(U.tile_sub[i], lv_pct(100), lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(U.tile_sub[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(U.tile_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        U.tile[i] = t;
    }
}

/* -------------------------------------------------------------------------- */
/* Fuente                                                                      */
/* -------------------------------------------------------------------------- */

static void psu_vset_done(double v) { aos_riden_set_v((float)v); }
static void psu_iset_done(double v) { aos_riden_set_i((float)v); }

static void psu_vset_cb(lv_event_t *e)
{
    aos_riden_status_t r;
    aos_riden_status(&r);
    keypad(_("Tensión de salida"), r.v_set, 2, "V", psu_vset_done);
}

static void psu_iset_cb(lv_event_t *e)
{
    aos_riden_status_t r;
    aos_riden_status(&r);
    keypad(_("Límite de corriente"), r.i_set, r.idec, "A", psu_iset_done);
}

static void psu_nudge_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);      /* tenths of a volt, or hundredths of an amp + 1000 */
    aos_riden_status_t r;
    aos_riden_status(&r);
    if (!r.ok) return;
    if (k >= 500) aos_riden_set_i(fmaxf(0, r.i_set + (k - 1000) / 100.0f));
    else aos_riden_set_v(fmaxf(0, r.v_set + k / 10.0f));
}

static void psu_out_cb(lv_event_t *e)
{
    aos_riden_status_t r;
    aos_riden_status(&r);
    if (r.ok) aos_riden_set_output(!r.on);
}

static void psu_refresh(void)
{
    if (!U.p_v) return;
    aos_riden_status_t r;
    aos_riden_status(&r);
    char t[48], a[24], b[24];
    if (r.model) lv_label_set_text_fmt(U.p_model, "RD%u · %s %u.%02u · SN %lu", r.model, _("firmware"), r.fw / 100, r.fw % 100, (unsigned long)r.serial);
    else lv_label_set_text(U.p_model, r.where[0] ? r.where : _("buscando la fuente…"));
    lv_label_set_text(U.p_err, r.err);
    if (!r.ok) {
        lv_label_set_text(U.p_v, "--,--");
        lv_obj_set_style_text_color(U.p_v, AOS_C_DIM, 0);
        lv_label_set_text(U.p_i, "-,-- A");
        lv_label_set_text(U.p_p, "-,-- W");
        lv_label_set_text(U.p_mode, "");
        lv_obj_set_style_bg_opa(U.p_mode, LV_OPA_TRANSP, 0);
        return;
    }
    lv_obj_set_style_bg_opa(U.p_mode, LV_OPA_COVER, 0);
    fmt_num(t, sizeof t, r.v_out, 2);
    lv_label_set_text(U.p_v, t);
    lv_obj_set_style_text_color(U.p_v, r.on ? (r.cc ? AOS_C_ORANGE : AOS_C_GREEN) : AOS_C_DIM, 0);
    fmt_unit(t, sizeof t, r.i_out, r.idec, "A");
    lv_label_set_text(U.p_i, t);
    fmt_unit(t, sizeof t, r.p_out, 2, "W");
    lv_label_set_text(U.p_p, t);
    lv_label_set_text(U.p_mode, !r.on ? _("APAGADA") : r.protect == 1 ? "OVP" : r.protect == 2 ? "OCP" : r.cc ? "CC" : "CV");
    lv_obj_set_style_bg_color(U.p_mode, !r.on ? AOS_C_CARD2 : r.protect ? AOS_C_RED : r.cc ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    lv_obj_set_style_bg_color(U.p_out, r.on ? AOS_C_GREEN : AOS_C_CARD2, 0);
    lv_label_set_text(U.p_out_l, r.on ? _("Salida encendida") : _("Salida apagada"));
    fmt_unit(t, sizeof t, r.v_set, 2, "V");
    lv_label_set_text(U.p_vset, t);
    fmt_unit(t, sizeof t, r.i_set, r.idec, "A");
    lv_label_set_text(U.p_iset, t);
    fmt_num(a, sizeof a, r.v_in - 1.01f > 0 ? r.v_in - 1.01f : 0, 2);
    fmt_num(b, sizeof b, r.v_in, 2);
    lv_label_set_text_fmt(U.p_cap, _("Tope %s V: la entrada es de %s V y la fuente no eleva."), a, b);
    fmt_unit(t, sizeof t, r.v_in, 2, "V");
    lv_label_set_text(U.p_fact[0], t);
    lv_label_set_text_fmt(U.p_fact[1], "%d °C", r.temp_c);
    fmt_unit(t, sizeof t, r.ah, 3, "Ah");
    lv_label_set_text(U.p_fact[2], t);
    fmt_unit(t, sizeof t, r.wh, 3, "Wh");
    lv_label_set_text(U.p_fact[3], t);
    lv_label_set_text(U.p_fact[4], r.protect == 1 ? "OVP" : r.protect == 2 ? "OCP" : _("ninguna"));
}

static void psu_chart_fill(void)
{
    if (!U.p_chart) return;
    float vmax = 1, imax = 0.1f;
    for (int i = 0; i < S.h_n; i++) { vmax = fmaxf(vmax, S.v_hist[i]); imax = fmaxf(imax, S.i_hist[i]); }
    lv_chart_set_axis_range(U.p_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)ceilf(vmax * 1.2f * 100));
    lv_chart_set_axis_range(U.p_chart, LV_CHART_AXIS_SECONDARY_Y, 0, (int32_t)ceilf(imax * 1.2f * 1000));
    int32_t *yv = lv_chart_get_series_y_array(U.p_chart, U.p_sv), *yi = lv_chart_get_series_y_array(U.p_chart, U.p_si);
    int off = PSU_HIST - S.h_n;
    for (int i = 0; i < PSU_HIST; i++) {
        yv[i] = i < off ? LV_CHART_POINT_NONE : (int32_t)lroundf(S.v_hist[i - off] * 100);
        yi[i] = i < off ? LV_CHART_POINT_NONE : (int32_t)lroundf(S.i_hist[i - off] * 1000);
    }
    lv_chart_refresh(U.p_chart);
}

/* Twice a second, whichever tab is open, so the trace has no hole. */
static void psu_sample(void)
{
    aos_riden_status_t r;
    aos_riden_status(&r);
    if (!r.ok || r.t_ms == S.rd_seen_t) return;
    S.rd_seen_t = r.t_ms;
    if (S.h_n == PSU_HIST) {
        memmove(S.v_hist, S.v_hist + 1, sizeof S.v_hist - sizeof S.v_hist[0]);
        memmove(S.i_hist, S.i_hist + 1, sizeof S.i_hist - sizeof S.i_hist[0]);
        S.h_n--;
    }
    S.v_hist[S.h_n] = r.v_out;
    S.i_hist[S.h_n] = r.i_out;
    S.h_n++;
    psu_chart_fill();
}

static lv_obj_t *setrow(lv_obj_t *parent, const char *label, lv_obj_t **value, lv_event_cb_t cb, bool line)
{
    lv_obj_t *r = box(parent, lv_pct(100), AOS_UI_ROW_H);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    if (line) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    lv_obj_align(aos_label(r, label, aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    *value = aos_label(r, "--", aos_font_title, C_AMBER);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_align(aos_label(r, AOS_SYM_PENCIL, &aos_sym_28, AOS_C_DIM), LV_ALIGN_RIGHT_MID, 0, 0);
    return r;
}

static void build_psu(void)
{
    int32_t w = U.cw;
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = (w - AOS_UI_PAD) * 11 / 20;
        rw = w - lw - AOS_UI_PAD;
        left = column(U.content, lw, U.ch);
        right = column(U.content, rw, U.ch);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(U.content, w, U.ch);
    }
    lv_obj_t *head = box(left, lw, 64);
    lv_obj_align(aos_label(head, "Riden", aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, -12);
    U.p_model = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.p_model, LV_ALIGN_BOTTOM_LEFT, 6, 0);

    lv_obj_t *big = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(big, 22, 0);
    lv_obj_set_flex_flow(big, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(big, 6, 0);
    lv_obj_t *vr = box(big, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vr, 12, 0);
    U.p_v = aos_label(vr, "--,--", U.land ? &aos_inter_num_96 : &aos_inter_num_144, AOS_C_DIM);
    aos_label(vr, "V", aos_font_large, AOS_C_DIM);
    U.p_mode = aos_label(vr, "", aos_font_caption, lv_color_white());
    lv_obj_set_style_radius(U.p_mode, 12, 0);
    lv_obj_set_style_pad_hor(U.p_mode, 12, 0);
    lv_obj_set_style_pad_ver(U.p_mode, 4, 0);
    lv_obj_set_style_margin_bottom(U.p_mode, U.land ? 18 : 28, 0);
    lv_obj_t *ir = box(big, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ir, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ir, 40, 0);
    U.p_i = aos_label(ir, "", aos_font_huge, AOS_C_TEXT);
    U.p_p = aos_label(ir, "", aos_font_huge, AOS_C_DIM);
    U.p_err = aos_label(big, "", aos_font_caption, AOS_C_ORANGE);

    U.p_out = box(left, lw, 96);
    lv_obj_set_style_radius(U.p_out, 48, 0);
    lv_obj_set_style_bg_opa(U.p_out, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(U.p_out, AOS_C_CARD2, 0);
    lv_obj_add_flag(U.p_out, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(U.p_out, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(U.p_out, psu_out_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *orow = box(U.p_out, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(orow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(orow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(orow, 14, 0);
    lv_obj_center(orow);
    aos_make_decorative(aos_label(orow, AOS_SYM_POWER, &aos_sym_44, lv_color_white()));
    U.p_out_l = aos_label(orow, _("Salida"), aos_font_title, lv_color_white());
    aos_make_decorative(orow);

    lv_obj_t *g = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    setrow(g, _("Tensión"), &U.p_vset, psu_vset_cb, false);
    lv_obj_t *nv = chips_row(g, lv_pct(100));
    lv_obj_set_style_pad_hor(nv, 18, 0);
    lv_obj_set_style_pad_bottom(nv, 16, 0);
    static const struct { const char *t; int k; } NV[] = { { "-1 V", -10 }, { "-0,1", -1 }, { "+0,1", 1 }, { "+1 V", 10 } };
    for (int i = 0; i < 4; i++) chip(nv, NV[i].t, false, AOS_C_TEXT, psu_nudge_cb, (void *)(intptr_t)NV[i].k);
    setrow(g, _("Corriente"), &U.p_iset, psu_iset_cb, true);
    lv_obj_t *ni = chips_row(g, lv_pct(100));
    lv_obj_set_style_pad_hor(ni, 18, 0);
    lv_obj_set_style_pad_bottom(ni, 16, 0);
    static const struct { const char *t; int k; } NI[] = { { "-0,1 A", 990 }, { "-0,01", 999 }, { "+0,01", 1001 }, { "+0,1 A", 1010 } };
    for (int i = 0; i < 4; i++) chip(ni, NI[i].t, false, AOS_C_TEXT, psu_nudge_cb, (void *)(intptr_t)NI[i].k);
    U.p_cap = caption(left, "", lw);

    lv_obj_t *fg = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(fg, 12, 0);
    static const char *const FACT[5] = { N_("Entrada"), N_("Temperatura"), "Ah", "Wh", N_("Protección") };
    int32_t fw = (rw - 2 * 12) / 3;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *c = card(fg, i < 3 ? fw : (rw - 12) / 2, 96);
        lv_obj_set_style_pad_all(c, 14, 0);
        lv_obj_align(aos_label(c, aos_tr(FACT[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        U.p_fact[i] = aos_label(c, "--", i < 3 && fw < 170 ? aos_font_small : aos_font_body, AOS_C_TEXT);
        lv_obj_align(U.p_fact[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lv_obj_t *leg = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(leg, 20, 0);
    lv_obj_set_style_pad_left(leg, 10, 0);
    aos_label(leg, _("ÚLTIMOS 2 MINUTOS"), aos_font_caption, AOS_C_DIM);
    aos_label(leg, "V", aos_font_caption, AOS_C_GREEN);
    aos_label(leg, "A", aos_font_caption, AOS_C_ORANGE);
    U.p_chart = lv_chart_create(right);
    lv_obj_set_size(U.p_chart, rw, U.land ? 240 : 220);
    lv_obj_set_style_bg_color(U.p_chart, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(U.p_chart, 0, 0);
    lv_obj_set_style_radius(U.p_chart, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(U.p_chart, 16, 0);
    lv_obj_set_style_line_color(U.p_chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(U.p_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(U.p_chart, PSU_HIST);
    lv_chart_set_div_line_count(U.p_chart, 4, 0);
    lv_obj_set_style_size(U.p_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.p_chart, 4, LV_PART_ITEMS);
    U.p_sv = lv_chart_add_series(U.p_chart, AOS_C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    U.p_si = lv_chart_add_series(U.p_chart, AOS_C_ORANGE, LV_CHART_AXIS_SECONDARY_Y);
    psu_chart_fill();
    caption(right, _("La fuente se conecta por su puerto TTL (3,3 V) al puerto que elegiste en Modbus, pestaña Riden; el enlace es el mismo. Cada valor que se escribe se vuelve a leer, porque la Riden rechaza en silencio lo que no le gusta."), rw);
    psu_refresh();
}

/* -------------------------------------------------------------------------- */
/* Generador                                                                   */
/* -------------------------------------------------------------------------- */

static void build_gen(void)
{
    int32_t w = U.cw;
    lv_obj_t *col = column(U.content, w, U.ch);
    lv_obj_t *head = box(col, w, 64);
    lv_obj_align(aos_label(head, "UNI-T UTG932E", aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, -12);
    lv_obj_align(aos_label(head, _("generador de funciones, 2 canales, 30 MHz"), aos_font_caption, AOS_C_DIM), LV_ALIGN_BOTTOM_LEFT, 6, 0);

    lv_obj_t *c = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *r = box(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    aos_label(r, AOS_SYM_USB, &aos_sym_44, AOS_C_ORANGE);
    aos_label(r, _("Necesita un hub USB con alimentación"), aos_font_body, AOS_C_TEXT);
    caption(c, _("El generador habla USBTMC por USB, sin red ni puerto serie. El puerto USB-OTG de la placa no entrega 5 V, así que el generador se enchufa a un hub alimentado y el hub a la placa. Cuando esté el hub, esta pestaña maneja los dos canales."), w - 48);

    section(col, _("LO QUE VA A HACER"));
    lv_obj_t *l = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(l, 22, 0);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 12, 0);
    static const char *const WHAT[] = {
        N_("Forma, frecuencia, amplitud, offset y fase de cada canal"),
        N_("Encender y apagar cada salida"),
        N_("Barridos y ráfagas"),
        N_("Arbitrarias propias desde la tarjeta"),
        N_("Mirar lo que sale en el osciloscopio de al lado"),
    };
    for (size_t i = 0; i < sizeof WHAT / sizeof WHAT[0]; i++) {
        lv_obj_t *row = box(l, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 14, 0);
        aos_label(row, AOS_SYM_CHECK, &aos_sym_28, AOS_C_DIM);
        lv_obj_t *t = aos_label(row, aos_tr(WHAT[i]), aos_font_small, AOS_C_TEXT);
        lv_obj_set_flex_grow(t, 1);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    }
    caption(col, _("Lo aprendido con el panel de la Mac: el firmware 1.07 no tiene cola de errores y descarta en silencio los comandos mal escritos, así que cada ajuste se va a releer, como con la Riden."), w);
}

/* -------------------------------------------------------------------------- */
/* Registro                                                                    */
/* -------------------------------------------------------------------------- */

static const uint32_t INTERVALS[] = { 500, 1000, 2000, 5000, 10000, 30000, 60000 };
static const char *const INTERVAL_T[] = { "0,5 s", "1 s", "2 s", "5 s", "10 s", "30 s", "1 min" };

static void log_series_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    bench_log_stat_t l;
    bench_log_stat(&l);
    if (l.running) { aos_ui_toast(_("Pará el registro para cambiar las columnas."), 2000); return; }
    bench_log_configure(l.mask ^ (1u << s), l.interval_ms);
    build_tab();
}

static void log_interval_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bench_log_stat_t l;
    bench_log_stat(&l);
    if (l.running) { aos_ui_toast(_("Pará el registro para cambiar el intervalo."), 2000); return; }
    bench_log_configure(l.mask, INTERVALS[i]);
    build_tab();
}

static void log_go_cb(lv_event_t *e)
{
    bench_log_stat_t l;
    bench_log_stat(&l);
    if (l.running) bench_log_stop();
    else if (!bench_log_start()) { bench_log_stat(&l); aos_ui_toast(l.err, 2500); }
}

static void log_focus_cb(lv_event_t *e)
{
    S.log_focus = (int)(intptr_t)lv_event_get_user_data(e);
    build_tab();
}

static void log_refresh(void)
{
    if (!U.l_time) return;
    bench_log_stat_t l;
    bench_log_stat(&l);
    uint32_t e = l.running ? ((uint32_t)aos_hal_uptime_ms() - l.started_ms) / 1000 : 0;
    lv_label_set_text_fmt(U.l_time, "%02u:%02u:%02u", (unsigned)(e / 3600), (unsigned)(e / 60 % 60), (unsigned)(e % 60));
    lv_obj_set_style_text_color(U.l_time, l.running ? AOS_C_TEXT : AOS_C_DIM, 0);
    if (l.running || l.rows) lv_label_set_text_fmt(U.l_rows, _("%u filas"), (unsigned)l.rows);
    else lv_label_set_text(U.l_rows, _("sin grabar"));
    lv_label_set_text(U.l_path, l.err[0] ? l.err : l.path[0] ? l.path : _("Cada toma, una fila de un CSV en la carpeta logs de la tarjeta."));
    lv_obj_set_style_text_color(U.l_path, l.err[0] ? AOS_C_ORANGE : AOS_C_DIM, 0);
    lv_obj_set_style_bg_color(U.l_btn, l.running ? AOS_C_RED : AOS_C_CARD2, 0);
    lv_label_set_text(U.l_btn_l, l.running ? _("Detener") : _("Grabar"));
    /* the chart and the legend's last values */
    if (U.l_chart && S.log_focus >= 0) {
        static float h[BL_HISTORY];
        int n = bench_log_history(S.log_focus, h, BL_HISTORY);
        float lo = INFINITY, hi = -INFINITY;
        for (int i = 0; i < n; i++) if (!isnan(h[i])) { lo = fminf(lo, h[i]); hi = fmaxf(hi, h[i]); }
        int32_t *y = lv_chart_get_series_y_array(U.l_chart, U.l_ser);
        if (lo > hi) { lo = 0; hi = 1; }
        float span = hi - lo, base;
        if (span < fabsf(hi) * 0.02f || span <= 0) {          /* a flat line: in the middle, not on the floor */
            span = fmaxf(fabsf(hi) * 0.02f, 1e-6f);
            base = (lo + hi) / 2 - span * 0.6f;
        } else {
            base = lo - span * 0.1f;
        }
        float scale = 1000.0f / (span * 1.2f);
        int off = BL_HISTORY - n;
        for (int i = 0; i < BL_HISTORY; i++)
            y[i] = i < off || isnan(h[i - off]) ? LV_CHART_POINT_NONE : (int32_t)lroundf((h[i - off] - base) * scale);
        lv_chart_set_axis_range(U.l_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
        lv_chart_refresh(U.l_chart);
        char a[32], b[32], c[32];
        const char *u = bench_log_series_unit(S.log_focus);
        if (n && !isnan(h[n - 1])) fmt_eng(c, sizeof c, h[n - 1], u); else snprintf(c, sizeof c, "--");
        if (lo <= hi && n) { fmt_eng(a, sizeof a, lo, u); fmt_eng(b, sizeof b, hi, u); }
        else { snprintf(a, sizeof a, "--"); snprintf(b, sizeof b, "--"); }
        lv_label_set_text_fmt(U.l_focus, "%s   %s   ·   %s %s   %s %s", bench_log_series_name(S.log_focus), c, _("mín"), a, _("máx"), b);
    }
}

static void build_log(void)
{
    int32_t w = U.cw;
    bench_log_stat_t l;
    bench_log_stat(&l);
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = (w - AOS_UI_PAD) / 2;
        rw = w - lw - AOS_UI_PAD;
        left = column(U.content, lw, U.ch);
        right = column(U.content, rw, U.ch);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(U.content, w, U.ch);
    }
    lv_obj_t *top = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top, 22, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(top, 8, 0);
    lv_obj_t *tr = box(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(tr, 12, 0);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *tl = box(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tl, LV_FLEX_FLOW_COLUMN);
    U.l_time = aos_label(tl, "00:00:00", aos_font_huge, AOS_C_DIM);
    U.l_rows = aos_label(tl, "", aos_font_small, AOS_C_DIM);
    U.l_btn = box(tr, 200, 96);
    lv_obj_set_style_radius(U.l_btn, 48, 0);
    lv_obj_set_style_bg_opa(U.l_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(U.l_btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(U.l_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.l_btn, log_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *br = box(U.l_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br, 10, 0);
    lv_obj_center(br);
    aos_make_decorative(aos_label(br, AOS_SYM_RECORD_REC, &aos_sym_28, lv_color_white()));
    U.l_btn_l = aos_label(br, "", aos_font_body, lv_color_white());
    aos_make_decorative(br);
    U.l_path = caption(top, "", lw - 44);

    section(left, _("QUÉ SE ANOTA"));
    lv_obj_t *sc = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sc, 18, 0);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sc, 12, 0);
    aos_scope_status_t st;
    aos_scope_status(&st);
    for (int g = 0; g < 5; g++) {
        lv_obj_t *row = box(sc, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        char name[16];
        if (g == 0) snprintf(name, sizeof name, "Riden");
        else snprintf(name, sizeof name, "CH%d%s", g, st.connected && !st.ch[g - 1].on ? "*" : "");
        lv_obj_t *n = aos_label(row, name, aos_font_small, g ? lv_color_hex(bench_log_series_color(BL_CH_FIRST + 3 * (g - 1))) : AOS_C_GREEN);
        lv_obj_set_width(n, 110);
        int first = g == 0 ? 0 : BL_CH_FIRST + 3 * (g - 1);
        for (int k = 0; k < 3; k++) {
            int s = first + k;
            const char *label = g == 0 ? (k == 0 ? "V" : k == 1 ? "A" : "W") : (k == 0 ? "Vpp" : k == 1 ? "Vavg" : "Freq");
            chip(row, label, l.mask & (1u << s), lv_color_hex(bench_log_series_color(s)), log_series_cb, (void *)(intptr_t)s);
        }
    }
    if (st.connected) caption(sc, _("* canal apagado en el osciloscopio: su columna queda vacía."), lw - 36);

    section(left, _("CADA"));
    lv_obj_t *ir = chips_row(left, lw);
    for (size_t i = 0; i < sizeof INTERVALS / sizeof INTERVALS[0]; i++)
        chip(ir, INTERVAL_T[i], l.interval_ms == INTERVALS[i], lv_color_hex(0xF2F2F7), log_interval_cb, (void *)(intptr_t)i);

    /* the chart of one series, and the chips to choose it */
    if (S.log_focus < 0 || !(l.mask & (1u << S.log_focus))) {
        S.log_focus = -1;
        for (int s = 0; s < BL_SERIES; s++) if (l.mask & (1u << s)) { S.log_focus = s; break; }
    }
    section(right, _("EN VIVO"));
    U.l_legend = chips_row(right, rw);
    for (int s = 0; s < BL_SERIES; s++)
        if (l.mask & (1u << s))
            chip(U.l_legend, bench_log_series_name(s), s == S.log_focus, lv_color_hex(bench_log_series_color(s)), log_focus_cb, (void *)(intptr_t)s);
    U.l_focus = aos_label(right, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_size(U.l_focus, rw, lv_font_get_line_height(aos_font_small));
    lv_label_set_long_mode(U.l_focus, LV_LABEL_LONG_MODE_DOTS);
    if (S.log_focus >= 0) {
        U.l_chart = lv_chart_create(right);
        lv_obj_set_size(U.l_chart, rw, U.land ? 300 : 260);
        lv_obj_set_style_bg_color(U.l_chart, AOS_C_CARD, 0);
        lv_obj_set_style_border_width(U.l_chart, 0, 0);
        lv_obj_set_style_radius(U.l_chart, AOS_UI_RADIUS, 0);
        lv_obj_set_style_pad_all(U.l_chart, 16, 0);
        lv_obj_set_style_line_color(U.l_chart, AOS_C_CARD2, LV_PART_MAIN);
        lv_chart_set_type(U.l_chart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(U.l_chart, BL_HISTORY);
        lv_chart_set_div_line_count(U.l_chart, 4, 0);
        lv_obj_set_style_size(U.l_chart, 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(U.l_chart, 4, LV_PART_ITEMS);
        U.l_ser = lv_chart_add_series(U.l_chart, lv_color_hex(bench_log_series_color(S.log_focus)), LV_CHART_AXIS_PRIMARY_Y);
    }
    caption(right, _("Un valor que el equipo no tiene en ese momento (fuente desenchufada, canal apagado, medida inválida) queda como celda vacía, nunca repetido. El registro sigue con la app cerrada."), rw);
    log_refresh();
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static void build_tab(void)
{
    kp_close();
    bench_scope_destroy();
    lv_obj_clean(U.content);
    U.p_v = U.p_chart = NULL;
    U.l_time = U.l_chart = NULL;
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_set_style_border_width(U.tile[i], i == S.tab ? 3 : 0, 0);
        lv_obj_set_style_bg_color(U.tile[i], i == S.tab ? lv_color_hex(0x2A1618) : AOS_C_CARD, 0);
    }
    if (S.tab == TAB_SCOPE) bench_scope_build(U.content, U.cw, U.ch, U.land);
    else if (S.tab == TAB_PSU) build_psu();
    else if (S.tab == TAB_GEN) build_gen();
    else build_log();
}

static void timer_cb(lv_timer_t *t)
{
    U.ticks++;
    aos_riden_keep(AOS_RIDEN_KEEP_UI);
    if (S.tab == TAB_SCOPE) bench_scope_tick();
    aos_riden_status_t r;
    aos_riden_status(&r);
    if (r.wseq != U.rd_wseq) {
        U.rd_wseq = r.wseq;
        if (r.wmsg[0]) aos_ui_toast(r.wmsg, 3000);
    }
    if (r.seq != U.rd_seq) {
        U.rd_seq = r.seq;
        if (S.tab == TAB_PSU) psu_refresh();
    }
    if (U.ticks % 5 == 0) {
        psu_sample();
        tiles_refresh();
        if (S.tab == TAB_LOG) log_refresh();
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    if (!S.loaded) {
        S.loaded = true;
        int32_t v;
        if (aos_hal_pref_get_i32("bench_tab", &v) && v >= 0 && v < TAB_COUNT) S.tab = v;
    }
    aos_scope_start();
    aos_riden_keep(AOS_RIDEN_KEEP_UI);
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    aos_riden_status_t r;
    aos_riden_status(&r);
    U.rd_wseq = r.wseq;
    const int32_t gap = 12;
    lv_obj_t *strip;
    if (U.land) {
        const int32_t sw = 250;
        strip = box(root, sw, U.H);
        lv_obj_set_style_pad_left(strip, AOS_UI_PAD, 0);
        lv_obj_set_style_pad_ver(strip, 8, 0);
        lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(strip, gap, 0);
        int32_t th = (U.H - 16 - 24 - 3 * gap) / TAB_COUNT;
        build_tiles(strip, sw - AOS_UI_PAD, th);
        U.cw = U.W - sw - 2 * AOS_UI_PAD;
        U.ch = U.H - 8;
        U.content = box(root, U.cw, U.ch);
        lv_obj_set_pos(U.content, sw + AOS_UI_PAD, 8);
    } else {
        const int32_t sh = 136;
        strip = box(root, U.W, sh);
        lv_obj_set_style_pad_hor(strip, AOS_UI_PAD, 0);
        lv_obj_set_style_pad_top(strip, 8, 0);
        lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(strip, gap, 0);
        int32_t tw = (U.W - 2 * AOS_UI_PAD - 3 * gap) / TAB_COUNT;
        build_tiles(strip, tw, sh - 8);
        U.cw = U.W - 2 * AOS_UI_PAD;
        U.ch = U.H - sh - 16;
        U.content = box(root, U.cw, U.ch);
        lv_obj_set_pos(U.content, AOS_UI_PAD, sh + 16);
    }
    tiles_refresh();
    build_tab();
    U.timer = lv_timer_create(timer_cb, 100, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    bench_scope_destroy();
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { kp_close(); return true; }
    if (S.tab == TAB_SCOPE && bench_scope_back()) return true;
    return false;
}

void aos_app_bench_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.bench", .name = "Banco", .icon = AOS_SYM_GAUGE,
            .color_a = 0xEF4444, .color_b = 0x991B1B,
            .order = 150,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
