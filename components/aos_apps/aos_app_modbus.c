/*
 * P4OS - Modbus: a master for the bench, over aos_modbus.c.
 *
 * Three tabs:
 *
 *   Riden     the RD60xx power supply (RTU, 115200, unit 1) as a panel: the
 *             output live, big; the setpoints, each write read back because
 *             the supply rejects what it does not like in silence and the
 *             app must not lie about it; the ceiling the input sets (a buck:
 *             input minus ~1 V); presets; a trace of the power. The register
 *             map is the one measured on the real unit (riden-psu/README.md).
 *   Gateway   a DOMCOM gateway over Modbus TCP: sensors, relays, aux
 *             modules, dimmers, magnetic sensors (its README's map).
 *   Explorar  any slave, RTU or TCP: a function, an address and a count,
 *             the values in the format you choose, live if you want, and any
 *             holding register or coil written with a tap.
 *
 * One worker thread owns the link and does all the talking; the screen
 * only reads what it left and queues writes. Leaving the app gives the port
 * back after a moment (a turn of the screen does not drop it).
 */
#include "aos_apps.h"
#include "aos_modbus.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"
#include "aos_mono.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OWNER      "Modbus"
#define C_AMBER    lv_color_hex(0xF59E0B)
#define C_TEAL     lv_color_hex(0x10B981)

enum { TAB_RIDEN, TAB_GW, TAB_EXP, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { "Riden", "Gateway", N_("Explorar") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_LIGHTNING_BOLT, AOS_SYM_LAN, AOS_SYM_MAGNIFY };

enum { PLAN_NONE, PLAN_RIDEN, PLAN_GW, PLAN_EXP };
enum { FMT_U16, FMT_S16, FMT_HEX, FMT_U32, FMT_S32, FMT_FLOAT, FMT_COUNT };
static const char *const FMT_NAME[FMT_COUNT] = { "U16", "S16", "HEX", "U32", "S32", "Float" };
static const char *const FC_NAME[5] = { "", "Coils", N_("Entradas"), "Holding", "Input" };

/* -------------------------------------------------------------------------- */
/* The worker                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t fc; uint16_t addr, n; uint16_t v[4]; char what[40]; } wreq_t;

static struct {
    void *mx;
    bool started;
    /* what the screen wants */
    int plan;
    uint32_t plan_ms;               /* the screen keeps this fresh while it is open */
    aos_mb_link_t want;
    uint32_t want_gen;
    int ex_fc;
    uint16_t ex_addr, ex_n;
    bool ex_live, ex_once;
    wreq_t q[8];
    int qn;
    /* what the worker found */
    bool linked;
    char err[96];
    aos_mb_stats_t st;
    uint32_t seq;
    uint16_t rd_id[4], rd_fast[16], rd_slow[10], rd_pre[40];
    bool rd_id_ok, rd_fast_ok, rd_slow_ok, rd_pre_ok;
    uint16_t gw_in[25], gw_coil[24], gw_hr[4], gw_di[2];
    bool gw_ok;
    uint16_t ex_val[125];
    int ex_rc, ex_got_fc;
    uint16_t ex_got_addr, ex_got_n;
    uint32_t ex_seq;
    char wmsg[96];
    bool wbad;
    uint32_t wseq;
} W;
/* Not initialised in place, so that it is .bss and goes to PSRAM (psram.lf). */
__attribute__((constructor)) static void W_defaults(void) { W.ex_fc = 3; W.ex_n = 10; }

static void w_lock(void) { aos_hal_mutex_lock(W.mx); }
static void w_unlock(void) { aos_hal_mutex_unlock(W.mx); }

static void w_err(int rc, const char *where)
{
    w_lock();
    snprintf(W.err, sizeof W.err, "%s: %s", where, aos_mb_strerror(rc));
    W.seq++;
    w_unlock();
}

/* Reads a block into dst under the lock; false on error (the error noted). */
static bool w_read(aos_mb_t *m, int fc, uint16_t a, uint16_t n, uint16_t *dst, bool *ok, const char *where)
{
    uint16_t tmp[125];
    int rc = aos_mb_read(m, fc, a, n, tmp);
    w_lock();
    if (!rc) { memcpy(dst, tmp, n * sizeof *tmp); if (ok) *ok = true; W.err[0] = 0; }
    aos_mb_stats(m, &W.st);
    W.seq++;
    w_unlock();
    if (rc) w_err(rc, where);
    return rc == 0;
}

static void worker(void *arg)
{
    (void)arg;
    aos_mb_t *m = NULL;
    uint32_t gen = 0, t_fast = 0, t_slow = 0, t_ex = 0;
    int plan_linked = PLAN_NONE;
    for (;;) {
        w_lock();
        int plan = W.plan;
        bool stale = (uint32_t)aos_hal_uptime_ms() - W.plan_ms > 2000;     /* the app went away */
        aos_mb_link_t want = W.want;
        uint32_t want_gen = W.want_gen;
        w_unlock();
        if (plan == PLAN_NONE || stale) {
            if (m) { aos_mb_close(m); m = NULL; w_lock(); W.linked = false; W.seq++; w_unlock(); }
            aos_hal_sleep_ms(100);
            continue;
        }
        if (m && (gen != want_gen || plan_linked != plan)) { aos_mb_close(m); m = NULL; }
        if (!m) {
            char err[96];
            m = aos_mb_open(&want, OWNER, err, sizeof err);
            w_lock();
            W.linked = m != NULL;
            W.rd_id_ok = W.rd_fast_ok = W.rd_slow_ok = W.rd_pre_ok = W.gw_ok = false;
            snprintf(W.err, sizeof W.err, "%s", m ? "" : err);
            W.seq++;
            w_unlock();
            gen = want_gen;
            plan_linked = plan;
            if (!m) { aos_hal_sleep_ms(1000); continue; }
            t_fast = t_slow = t_ex = 0;
        }
        /* writes first */
        w_lock();
        wreq_t q = W.q[0];
        bool have = W.qn > 0;
        if (have) { memmove(W.q, W.q + 1, (size_t)(W.qn - 1) * sizeof W.q[0]); W.qn--; }
        w_unlock();
        if (have) {
            int rc = q.fc == 5 ? aos_mb_write_coil(m, q.addr, q.v[0])
                   : q.fc == 6 ? aos_mb_write_reg(m, q.addr, q.v[0])
                               : aos_mb_write_regs(m, q.addr, q.n, q.v);
            char msg[96] = "";
            bool bad = rc != 0;
            if (rc) snprintf(msg, sizeof msg, "%s: %s", q.what, aos_mb_strerror(rc));
            else if (plan == PLAN_RIDEN && q.fc != 5) {
                /* the Riden refuses in silence: read it back */
                uint16_t back[4];
                if (!aos_mb_read(m, 3, q.addr, q.n, back) && memcmp(back, q.v, q.n * sizeof back[0])) {
                    snprintf(msg, sizeof msg, _("La fuente no aceptó %s"), q.what);
                    bad = true;
                }
            }
            w_lock();
            snprintf(W.wmsg, sizeof W.wmsg, "%s", msg);
            W.wbad = bad;
            W.wseq++;
            w_unlock();
            t_fast = t_slow = 0;            /* show the effect at once */
            continue;
        }
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        bool io_fail = false;
        if (plan == PLAN_RIDEN) {
            if (!W.rd_id_ok) io_fail |= !w_read(m, 3, 0, 4, W.rd_id, &W.rd_id_ok, _("modelo"));
            else if (!W.rd_pre_ok) io_fail |= !w_read(m, 3, 80, 40, W.rd_pre, &W.rd_pre_ok, _("presets"));
            else if (now - t_fast >= 250) { t_fast = now; io_fail |= !w_read(m, 3, 4, 16, W.rd_fast, &W.rd_fast_ok, _("lectura")); }
            else if (now - t_slow >= 2000) { t_slow = now; io_fail |= !w_read(m, 3, 32, 10, W.rd_slow, &W.rd_slow_ok, _("contadores")); }
        } else if (plan == PLAN_GW) {
            if (now - t_fast >= 500) {
                t_fast = now;
                bool ok = w_read(m, 4, 0, 25, W.gw_in, NULL, _("sensores")) &&
                          w_read(m, 1, 0, 4, W.gw_coil, NULL, _("relés")) &&
                          w_read(m, 1, 20, 4, W.gw_coil + 20, NULL, _("módulos")) &&
                          w_read(m, 3, 0, 4, W.gw_hr, NULL, _("dimmers")) &&
                          w_read(m, 2, 8, 2, W.gw_di, NULL, _("magnéticos"));
                w_lock(); W.gw_ok = ok; w_unlock();
                io_fail |= !ok;
            }
        } else if (plan == PLAN_EXP) {
            w_lock();
            bool go = W.ex_once || (W.ex_live && now - t_ex >= 500);
            int fc = W.ex_fc;
            uint16_t a = W.ex_addr, n = W.ex_n;
            W.ex_once = false;
            w_unlock();
            if (go) {
                t_ex = now;
                uint16_t tmp[125];
                int rc = aos_mb_read(m, fc, a, n, tmp);
                w_lock();
                W.ex_rc = rc;
                if (!rc) { memcpy(W.ex_val, tmp, n * sizeof *tmp); W.ex_got_fc = fc; W.ex_got_addr = a; W.ex_got_n = n; }
                aos_mb_stats(m, &W.st);
                W.ex_seq++;
                W.seq++;
                w_unlock();
                io_fail |= rc == AOS_MB_E_IO;
            }
        }
        if (io_fail && W.err[0] && strstr(W.err, aos_mb_strerror(AOS_MB_E_IO))) { aos_mb_close(m); m = NULL; }
        aos_hal_sleep_ms(15);
    }
}

static void w_start(void)
{
    if (W.started) return;
    W.mx = aos_hal_mutex_create();
    W.started = aos_hal_thread_start("modbus", worker, NULL, 6144, 4);
}

static void w_plan(int plan, const aos_mb_link_t *link)
{
    w_lock();
    if (link && memcmp(link, &W.want, sizeof *link)) { W.want = *link; W.want_gen++; }
    W.plan = plan;
    W.plan_ms = (uint32_t)aos_hal_uptime_ms();
    w_unlock();
}

static bool w_write(uint8_t fc, uint16_t addr, uint16_t n, const uint16_t *v, const char *what)
{
    w_lock();
    bool ok = W.qn < 8;
    if (ok) {
        wreq_t *q = &W.q[W.qn++];
        q->fc = fc;
        q->addr = addr;
        q->n = n;
        memcpy(q->v, v, n * sizeof *v);
        snprintf(q->what, sizeof q->what, "%s", what);
    }
    w_unlock();
    return ok;
}

/* -------------------------------------------------------------------------- */
/* State kept across rotations, and the settings                               */
/* -------------------------------------------------------------------------- */

static struct {
    int tab;
    char rd_port[16];
    uint32_t rd_baud;
    uint8_t rd_unit;
    char gw_host[64];
    int gw_port;
    uint8_t gw_unit;
    aos_mb_link_t ex;
    int ex_fmt;
    bool loaded;
    float p_hist[120];
    int p_n;
} S;

static void settings_load(void)
{
    if (S.loaded) return;
    S.loaded = true;
    int32_t v;
    if (!aos_hal_pref_get_str("mb_rd_port", S.rd_port, sizeof S.rd_port)) snprintf(S.rd_port, sizeof S.rd_port, "uart.a");
    S.rd_baud = aos_hal_pref_get_i32("mb_rd_baud", &v) ? (uint32_t)v : 115200;
    S.rd_unit = aos_hal_pref_get_i32("mb_rd_unit", &v) ? (uint8_t)v : 1;
    if (!aos_hal_pref_get_str("mb_gw_host", S.gw_host, sizeof S.gw_host)) S.gw_host[0] = 0;
    S.gw_port = aos_hal_pref_get_i32("mb_gw_port", &v) ? v : 502;
    S.gw_unit = aos_hal_pref_get_i32("mb_gw_unit", &v) ? (uint8_t)v : 1;
    S.ex.transport = aos_hal_pref_get_i32("mb_ex_tr", &v) ? v : AOS_MB_RTU;
    if (!aos_hal_pref_get_str("mb_ex_port", S.ex.port, sizeof S.ex.port)) snprintf(S.ex.port, sizeof S.ex.port, "uart.a");
    S.ex.baud = aos_hal_pref_get_i32("mb_ex_baud", &v) ? (uint32_t)v : 9600;
    S.ex.parity = aos_hal_pref_get_i32("mb_ex_par", &v) && (v == 'E' || v == 'O') ? (char)v : 'N';
    S.ex.stop_bits = aos_hal_pref_get_i32("mb_ex_stop", &v) && v == 2 ? 2 : 1;
    if (!aos_hal_pref_get_str("mb_ex_host", S.ex.host, sizeof S.ex.host)) S.ex.host[0] = 0;
    S.ex.tcp_port = aos_hal_pref_get_i32("mb_ex_tport", &v) ? v : 502;
    S.ex.unit = aos_hal_pref_get_i32("mb_ex_unit", &v) ? (uint8_t)v : 1;
    S.ex_fmt = aos_hal_pref_get_i32("mb_ex_fmt", &v) ? v : FMT_U16;
    if (aos_hal_pref_get_i32("mb_tab", &v)) S.tab = v;
    int32_t fc = 3, a = 0, n = 10;
    aos_hal_pref_get_i32("mb_ex_fc", &fc);
    aos_hal_pref_get_i32("mb_ex_addr", &a);
    aos_hal_pref_get_i32("mb_ex_n", &n);
    W.ex_fc = (int)fc;
    W.ex_addr = (uint16_t)a;
    W.ex_n = (uint16_t)n;
}

static void settings_save(void)
{
    aos_hal_pref_set_str("mb_rd_port", S.rd_port);
    aos_hal_pref_set_i32("mb_rd_baud", (int32_t)S.rd_baud);
    aos_hal_pref_set_i32("mb_rd_unit", S.rd_unit);
    aos_hal_pref_set_str("mb_gw_host", S.gw_host);
    aos_hal_pref_set_i32("mb_gw_port", S.gw_port);
    aos_hal_pref_set_i32("mb_gw_unit", S.gw_unit);
    aos_hal_pref_set_i32("mb_ex_tr", S.ex.transport);
    aos_hal_pref_set_str("mb_ex_port", S.ex.port);
    aos_hal_pref_set_i32("mb_ex_baud", (int32_t)S.ex.baud);
    aos_hal_pref_set_i32("mb_ex_par", S.ex.parity);
    aos_hal_pref_set_i32("mb_ex_stop", S.ex.stop_bits);
    aos_hal_pref_set_str("mb_ex_host", S.ex.host);
    aos_hal_pref_set_i32("mb_ex_tport", S.ex.tcp_port);
    aos_hal_pref_set_i32("mb_ex_unit", S.ex.unit);
    aos_hal_pref_set_i32("mb_ex_fmt", S.ex_fmt);
    aos_hal_pref_set_i32("mb_ex_fc", W.ex_fc);
    aos_hal_pref_set_i32("mb_ex_addr", W.ex_addr);
    aos_hal_pref_set_i32("mb_ex_n", W.ex_n);
    aos_hal_pref_set_i32("mb_tab", S.tab);
}

static aos_mb_link_t link_for_tab(void)
{
    aos_mb_link_t l = { 0 };
    if (S.tab == TAB_RIDEN) {
        l.transport = AOS_MB_RTU;
        snprintf(l.port, sizeof l.port, "%s", S.rd_port);
        l.baud = S.rd_baud;
        l.unit = S.rd_unit;
        l.gap_ms = 20;              /* the Riden stops answering without it */
        l.timeout_ms = 300;
        l.retries = 3;
    } else if (S.tab == TAB_GW) {
        l.transport = AOS_MB_TCP;
        snprintf(l.host, sizeof l.host, "%s", S.gw_host);
        l.tcp_port = S.gw_port;
        l.unit = S.gw_unit;
    } else {
        l = S.ex;
    }
    return l;
}

/* -------------------------------------------------------------------------- */
/* The screen                                                                  */
/* -------------------------------------------------------------------------- */

static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay, *status;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    uint32_t seen, wseen, exseen;
    /* Riden */
    lv_obj_t *rd_v, *rd_i, *rd_p, *rd_mode, *rd_out, *rd_out_l, *rd_vset, *rd_iset, *rd_cap, *rd_info[6], *rd_chart, *rd_model;
    lv_chart_series_t *rd_ser;
    lv_obj_t *rd_pre[9];
    /* Gateway */
    lv_obj_t *gw_sens[10], *gw_relay[8], *gw_dim[4], *gw_dim_l[4], *gw_mag[2];
    bool gw_drag;
    /* Explorer */
    lv_obj_t *ex_list, *ex_msg;
    /* the keypad */
    char kp_text[16];
    lv_obj_t *kp_value, *ta, *kb;
    void (*kp_done)(double v);
    int kp_dec;
    bool kp_fresh;
} U;

static void build_page(void);

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

static lv_obj_t *pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, lv_color_white()));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
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

static void status_refresh(void)
{
    if (!U.status) return;
    aos_mb_link_t lt = link_for_tab();
    if (lt.transport == AOS_MB_TCP && !lt.host[0]) {
        lv_label_set_text(U.status, _("Falta la dirección del equipo."));
        lv_obj_set_style_text_color(U.status, AOS_C_DIM, 0);
        return;
    }
    w_lock();
    bool linked = W.linked;
    char err[96];
    snprintf(err, sizeof err, "%s", W.err);
    aos_mb_stats_t st = W.st;
    w_unlock();
    aos_mb_link_t l = link_for_tab();
    char where[80];
    if (l.transport == AOS_MB_RTU) snprintf(where, sizeof where, "%s · %u 8%c%u · %s %u", l.port, (unsigned)(l.baud ? l.baud : 9600),
                                             l.parity ? l.parity : 'N', (unsigned)(l.stop_bits ? l.stop_bits : 1), _("unidad"), l.unit ? l.unit : 1);
    else snprintf(where, sizeof where, "%s:%d · %s %u", l.host[0] ? l.host : "?", l.tcp_port ? l.tcp_port : 502, _("unidad"), l.unit ? l.unit : 1);
    if (!linked || err[0]) {
        lv_label_set_text_fmt(U.status, "%s   %s", where, err[0] ? err : _("conectando…"));
        lv_obj_set_style_text_color(U.status, AOS_C_ORANGE, 0);
    } else {
        lv_label_set_text_fmt(U.status, "%s   %u ms · %u/%u", where, (unsigned)st.last_ms, (unsigned)st.answers, (unsigned)st.requests);
        lv_obj_set_style_text_color(U.status, AOS_C_DIM, 0);
    }
}

/* ---- the numeric keypad ---- */

static void kp_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.kp_value = U.ta = U.kb = NULL;
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

/* ---- a text field (host names) ---- */

static void (*s_text_done)(const char *);

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[64];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(U.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    kp_close();
    if (done) done(v);
}

static void text_entry(const char *title, const char *value, void (*done)(const char *))
{
    kp_close();
    s_text_done = done;
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 30);
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_text(U.ta, value);
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 100);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the connection card, shared by the tabs ---- */

static const uint32_t BAUDS[] = { 9600, 19200, 38400, 57600, 115200 };

static void port_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    char *dst = S.tab == TAB_RIDEN ? S.rd_port : S.ex.port;
    snprintf(dst, 16, "%s", name);
    settings_save();
    build_page();
}

static void baud_cb(lv_event_t *e)
{
    uint32_t b = BAUDS[(int)(intptr_t)lv_event_get_user_data(e)];
    if (S.tab == TAB_RIDEN) S.rd_baud = b; else S.ex.baud = b;
    settings_save();
    build_page();
}

static void unit_done(double v)
{
    uint8_t u = v < 1 ? 1 : v > 247 ? 247 : (uint8_t)v;
    if (S.tab == TAB_RIDEN) S.rd_unit = u; else if (S.tab == TAB_GW) S.gw_unit = u; else S.ex.unit = u;
    settings_save();
    build_page();
}

static void unit_cb(lv_event_t *e)
{
    aos_mb_link_t l = link_for_tab();
    keypad(_("Unidad (esclavo)"), l.unit ? l.unit : 1, 0, "", unit_done);
}

static void host_done(const char *h)
{
    char host[64];
    snprintf(host, sizeof host, "%s", h);
    int port = 502;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1) > 0 ? atoi(c + 1) : 502; }
    if (S.tab == TAB_GW) { snprintf(S.gw_host, sizeof S.gw_host, "%s", host); S.gw_port = port; }
    else { snprintf(S.ex.host, sizeof S.ex.host, "%s", host); S.ex.tcp_port = port; }
    settings_save();
    build_page();
}

static void host_cb(lv_event_t *e)
{
    aos_mb_link_t l = link_for_tab();
    char v[80] = "";
    if (l.host[0]) snprintf(v, sizeof v, "%s:%d", l.host, l.tcp_port ? l.tcp_port : 502);
    text_entry(_("Equipo Modbus TCP (dirección:puerto)"), v, host_done);
}

/* 8N1, 8E1, 8O1, 8N2: what meters and PLCs actually use */
static const struct { char par; uint8_t stop; const char *name; } FORMATS[] = {
    { 'N', 1, "8N1" }, { 'E', 1, "8E1" }, { 'O', 1, "8O1" }, { 'N', 2, "8N2" },
};

static void format_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    S.ex.parity = FORMATS[i].par;
    S.ex.stop_bits = FORMATS[i].stop;
    settings_save();
    build_page();
}

static void transport_cb(lv_event_t *e)
{
    S.ex.transport = (int)(intptr_t)lv_event_get_user_data(e);
    settings_save();
    build_page();
}

static void connection_card(lv_obj_t *parent, int32_t w, bool can_tcp)
{
    aos_mb_link_t l = link_for_tab();
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    if (can_tcp) {
        lv_obj_t *r = chips_row(c, w - 36);
        chip(r, "RTU", l.transport == AOS_MB_RTU, transport_cb, (void *)AOS_MB_RTU);
        chip(r, "TCP", l.transport == AOS_MB_TCP, transport_cb, (void *)AOS_MB_TCP);
    }
    lv_obj_t *r = chips_row(c, w - 36);
    if (l.transport == AOS_MB_RTU) {
        for (int i = 0; i < aos_io_port_count(); i++) {
            const aos_io_port_t *p = aos_io_port_at(i);
            if (p->kind != AOS_PORT_UART) continue;
            char t[32];
            snprintf(t, sizeof t, "%s%s", p->name, p->pins[2] >= 0 ? " · RS485" : "");
            chip(r, t, !strcmp(p->name, l.port), port_cb, (void *)p->name);
        }
        lv_obj_t *r2 = chips_row(c, w - 36);
        for (size_t i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) {
            char t[12];
            snprintf(t, sizeof t, "%u", (unsigned)BAUDS[i]);
            chip(r2, t, l.baud == BAUDS[i], baud_cb, (void *)(intptr_t)i);
        }
        char t[24];
        snprintf(t, sizeof t, "%s %u", _("Unidad"), l.unit ? l.unit : 1);
        chip(r2, t, false, unit_cb, NULL);
        if (S.tab == TAB_EXP) {             /* the Riden is 8N1, full stop */
            lv_obj_t *r3 = chips_row(c, w - 36);
            char par = l.parity ? l.parity : 'N';
            int stop = l.stop_bits ? l.stop_bits : 1;
            for (size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0]; i++)
                chip(r3, FORMATS[i].name, FORMATS[i].par == par && FORMATS[i].stop == stop, format_cb, (void *)(intptr_t)i);
        }
    } else {
        char t[96];
        snprintf(t, sizeof t, "%s", l.host[0] ? l.host : _("tocá para poner la dirección"));
        if (l.host[0]) snprintf(t, sizeof t, "%s:%d", l.host, l.tcp_port ? l.tcp_port : 502);
        chip(r, t, false, host_cb, NULL);
        char u[24];
        snprintf(u, sizeof u, "%s %u", _("Unidad"), l.unit ? l.unit : 1);
        chip(r, u, false, unit_cb, NULL);
    }
    U.status = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.status, w - 36);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_MODE_WRAP);
    status_refresh();
}

/* -------------------------------------------------------------------------- */
/* Riden                                                                       */
/* -------------------------------------------------------------------------- */

static double rd_iscale(void) { return W.rd_id_ok && W.rd_id[0] / 10 == 6006 ? 1000.0 : 100.0; }

static void rd_vset_done(double v)
{
    uint16_t r = (uint16_t)lround(v * 100);
    char what[40];
    fmt_num(what, sizeof what, v, 2);
    strcat(what, " V");
    w_write(6, 8, 1, &r, what);
}

static void rd_iset_done(double v)
{
    uint16_t r = (uint16_t)lround(v * rd_iscale());
    char what[40];
    fmt_num(what, sizeof what, v, rd_iscale() > 100 ? 3 : 2);
    strcat(what, " A");
    w_write(6, 9, 1, &r, what);
}

static void rd_vset_cb(lv_event_t *e)
{
    w_lock();
    double v = W.rd_fast[4] / 100.0;
    w_unlock();
    keypad(_("Tensión de salida"), v, 2, "V", rd_vset_done);
}

static void rd_iset_cb(lv_event_t *e)
{
    w_lock();
    double v = W.rd_fast[5] / rd_iscale();
    w_unlock();
    keypad(_("Límite de corriente"), v, rd_iscale() > 100 ? 3 : 2, "A", rd_iset_done);
}

static void rd_out_cb(lv_event_t *e)
{
    w_lock();
    uint16_t on = W.rd_fast[14] ? 0 : 1;
    w_unlock();
    w_write(6, 18, 1, &on, on ? _("encender") : _("apagar"));
}

static void rd_pre_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    w_lock();
    uint16_t v[2] = { W.rd_pre[4 * m], W.rd_pre[4 * m + 1] };
    w_unlock();
    char what[40];
    snprintf(what, sizeof what, "M%d", m);
    w_write(16, 8, 2, v, what);
}

static void rd_refresh(void)
{
    if (!U.rd_v) return;
    w_lock();
    bool ok = W.rd_fast_ok;
    uint16_t f[16], s[10], id[4], pre[40];
    memcpy(f, W.rd_fast, sizeof f);
    memcpy(s, W.rd_slow, sizeof s);
    memcpy(id, W.rd_id, sizeof id);
    memcpy(pre, W.rd_pre, sizeof pre);
    bool sok = W.rd_slow_ok, idok = W.rd_id_ok, pok = W.rd_pre_ok;
    w_unlock();
    double is = rd_iscale();
    int idec = is > 100 ? 3 : 2;
    char t[32];
    if (idok) {
        lv_label_set_text_fmt(U.rd_model, "RD%u · %s %u.%02u · SN %lu", id[0] / 10, _("firmware"), id[3] / 100, id[3] % 100,
                              (unsigned long)((uint32_t)id[1] << 16 | id[2]));
    }
    if (!ok) {
        lv_label_set_text(U.rd_v, "--,--");
        lv_label_set_text(U.rd_i, "-,-- A");
        lv_label_set_text(U.rd_p, "-,-- W");
        return;
    }
    double vout = f[6] / 100.0, iout = f[7] / is, p = ((uint32_t)f[8] << 16 | f[9]) / 100.0, vin = f[10] / 100.0;
    bool on = f[14] != 0, cc = f[13] != 0;
    fmt_num(t, sizeof t, vout, 2);
    lv_label_set_text(U.rd_v, t);
    lv_obj_set_style_text_color(U.rd_v, on ? (cc ? AOS_C_ORANGE : AOS_C_GREEN) : AOS_C_DIM, 0);
    fmt_num(t, sizeof t, iout, idec);
    strcat(t, " A");
    lv_label_set_text(U.rd_i, t);
    fmt_num(t, sizeof t, p, 2);
    strcat(t, " W");
    lv_label_set_text(U.rd_p, t);
    lv_label_set_text(U.rd_mode, !on ? _("SALIDA APAGADA") : f[12] == 1 ? "OVP" : f[12] == 2 ? "OCP" : cc ? "CC" : "CV");
    lv_obj_set_style_bg_color(U.rd_mode, !on ? AOS_C_CARD2 : f[12] ? AOS_C_RED : cc ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    lv_obj_set_style_bg_color(U.rd_out, on ? AOS_C_GREEN : AOS_C_CARD2, 0);
    lv_label_set_text(U.rd_out_l, on ? _("Salida encendida") : _("Salida apagada"));
    fmt_num(t, sizeof t, f[4] / 100.0, 2);
    strcat(t, " V");
    lv_label_set_text(U.rd_vset, t);
    fmt_num(t, sizeof t, f[5] / is, idec);
    strcat(t, " A");
    lv_label_set_text(U.rd_iset, t);
    char a[24], b[24];
    fmt_num(a, sizeof a, vin - 1.01 > 0 ? vin - 1.01 : 0, 2);
    fmt_num(b, sizeof b, vin, 2);
    lv_label_set_text_fmt(U.rd_cap, _("Tope %s V: la entrada es de %s V y la fuente no eleva."), a, b);
    /* the small facts */
    fmt_num(a, sizeof a, vin, 2);
    lv_label_set_text_fmt(U.rd_info[0], "%s V", a);
    lv_label_set_text_fmt(U.rd_info[1], "%s%u °C", f[0] ? "-" : "", f[1]);
    if (sok) {
        fmt_num(a, sizeof a, ((uint32_t)s[6] << 16 | s[7]) / 1000.0, 3);
        lv_label_set_text_fmt(U.rd_info[2], "%s Ah", a);
        fmt_num(a, sizeof a, ((uint32_t)s[8] << 16 | s[9]) / 1000.0, 3);
        lv_label_set_text_fmt(U.rd_info[3], "%s Wh", a);
    }
    lv_label_set_text(U.rd_info[4], f[12] == 1 ? "OVP" : f[12] == 2 ? "OCP" : _("ninguna"));
    lv_label_set_text(U.rd_info[5], f[11] ? _("bloqueado") : _("libre"));
    for (int m = 1; m <= 9 && pok; m++) {
        if (!U.rd_pre[m - 1]) continue;
        char v[16], i[16];
        fmt_num(v, sizeof v, pre[4 * m] / 100.0, 2);
        fmt_num(i, sizeof i, pre[4 * m + 1] / is, idec);
        lv_label_set_text_fmt(lv_obj_get_child(U.rd_pre[m - 1], 0), "M%d  %s V  %s A", m, v, i);
    }
}

static void rd_chart_push(void)
{
    if (!U.rd_chart) return;
    w_lock();
    bool ok = W.rd_fast_ok;
    double p = ((uint32_t)W.rd_fast[8] << 16 | W.rd_fast[9]) / 100.0;
    w_unlock();
    if (!ok) return;
    lv_chart_set_next_value(U.rd_chart, U.rd_ser, (int32_t)lround(p * 10));
    /* scale to what is there */
    int32_t mx = 10;
    int32_t *y = lv_chart_get_series_y_array(U.rd_chart, U.rd_ser);
    for (uint32_t i = 0; i < lv_chart_get_point_count(U.rd_chart); i++) if (y[i] != LV_CHART_POINT_NONE && y[i] > mx) mx = y[i];
    lv_chart_set_axis_range(U.rd_chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx + mx / 5);
}

static lv_obj_t *setrow(lv_obj_t *parent, const char *label, lv_obj_t **value, lv_event_cb_t cb)
{
    lv_obj_t *r = box(parent, lv_pct(100), AOS_UI_ROW_H);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(aos_label(r, label, aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    *value = aos_label(r, "--", aos_font_title, C_AMBER);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_align(aos_label(r, AOS_SYM_PENCIL, &aos_sym_28, AOS_C_DIM), LV_ALIGN_RIGHT_MID, 0, 0);
    return r;
}

static void build_riden(lv_obj_t *parent, int32_t ch)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = 600;
        rw = w - lw - AOS_UI_PAD;
        left = column(parent, lw, ch);
        right = column(parent, rw, ch);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(parent, w, ch);
    }
    lv_obj_t *head = box(left, lw, 88);
    lv_obj_align(aos_label(head, "Riden", aos_font_large, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, -10);
    U.rd_model = aos_label(head, _("buscando la fuente…"), aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.rd_model, LV_ALIGN_BOTTOM_LEFT, 6, 0);

    /* the output */
    lv_obj_t *big = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(big, 22, 0);
    lv_obj_set_flex_flow(big, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(big, 6, 0);
    lv_obj_t *vr = box(big, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vr, 12, 0);
    U.rd_v = aos_label(vr, "--,--", &aos_inter_num_144, AOS_C_DIM);
    aos_label(vr, "V", aos_font_large, AOS_C_DIM);
    U.rd_mode = aos_label(vr, "", aos_font_caption, lv_color_white());
    lv_obj_set_style_bg_opa(U.rd_mode, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.rd_mode, 12, 0);
    lv_obj_set_style_pad_hor(U.rd_mode, 12, 0);
    lv_obj_set_style_pad_ver(U.rd_mode, 4, 0);
    lv_obj_set_style_margin_bottom(U.rd_mode, 28, 0);
    lv_obj_t *ir = box(big, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ir, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ir, 40, 0);
    U.rd_i = aos_label(ir, "", aos_font_huge, AOS_C_TEXT);
    U.rd_p = aos_label(ir, "", aos_font_huge, AOS_C_DIM);

    U.rd_out = box(left, lw, 100);
    lv_obj_set_style_radius(U.rd_out, 50, 0);
    lv_obj_set_style_bg_opa(U.rd_out, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(U.rd_out, AOS_C_CARD2, 0);
    lv_obj_add_flag(U.rd_out, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(U.rd_out, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(U.rd_out, rd_out_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *orow = box(U.rd_out, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(orow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(orow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(orow, 14, 0);
    lv_obj_center(orow);
    aos_make_decorative(aos_label(orow, AOS_SYM_POWER, &aos_sym_44, lv_color_white()));
    U.rd_out_l = aos_label(orow, _("Salida"), aos_font_title, lv_color_white());
    aos_make_decorative(orow);

    lv_obj_t *g = card(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    setrow(g, _("Tensión"), &U.rd_vset, rd_vset_cb);
    lv_obj_t *r2 = setrow(g, _("Corriente"), &U.rd_iset, rd_iset_cb);
    lv_obj_set_style_border_side(r2, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r2, 1, 0);
    lv_obj_set_style_border_color(r2, AOS_C_CARD2, 0);
    U.rd_cap = caption(right, "", rw);

    /* facts */
    lv_obj_t *fg = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(fg, 12, 0);
    static const char *const FACT[6] = { N_("Entrada"), N_("Temperatura"), "Ah", "Wh", N_("Protección"), N_("Teclado") };
    int fcols = rw > 600 ? 3 : 3;
    int32_t fw = (rw - (fcols - 1) * 12) / fcols;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *c = card(fg, fw, 100);
        lv_obj_set_style_pad_all(c, 14, 0);
        lv_obj_align(aos_label(c, aos_tr(FACT[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        U.rd_info[i] = aos_label(c, "--", aos_font_body, AOS_C_TEXT);
        lv_obj_align(U.rd_info[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    section(right, _("POTENCIA, ÚLTIMOS 60 s"));
    U.rd_chart = lv_chart_create(right);
    lv_obj_set_size(U.rd_chart, rw, U.land ? 150 : 200);
    lv_obj_set_style_bg_color(U.rd_chart, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(U.rd_chart, 0, 0);
    lv_obj_set_style_radius(U.rd_chart, AOS_UI_RADIUS, 0);
    lv_obj_set_style_line_color(U.rd_chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(U.rd_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(U.rd_chart, 240);
    lv_chart_set_div_line_count(U.rd_chart, 4, 0);
    lv_obj_set_style_size(U.rd_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.rd_chart, 4, LV_PART_ITEMS);
    U.rd_ser = lv_chart_add_series(U.rd_chart, C_AMBER, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(U.rd_chart, U.rd_ser, LV_CHART_POINT_NONE);
    for (int i = 0; i < S.p_n; i++) lv_chart_set_next_value(U.rd_chart, U.rd_ser, (int32_t)lround(S.p_hist[i] * 10));

    section(right, _("PRESETS: TOCÁ UNO PARA CARGARLO"));
    lv_obj_t *pr = chips_row(right, rw);
    for (int m = 1; m <= 9; m++) {
        char t[8];
        snprintf(t, sizeof t, "M%d", m);
        U.rd_pre[m - 1] = chip(pr, t, false, rd_pre_cb, (void *)(intptr_t)m);
    }
    section(right, _("CONEXIÓN"));
    connection_card(right, rw, false);
    caption(right, _("La fuente habla por su puerto TTL a 115200 (3,3 V): TX, RX y GND al puerto elegido. Cada valor que se escribe se vuelve a leer, porque la Riden rechaza en silencio lo que no le gusta."), rw);
    rd_refresh();
}

/* -------------------------------------------------------------------------- */
/* Gateway                                                                     */
/* -------------------------------------------------------------------------- */

static void gw_relay_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int addr = k < 4 ? k : 20 + (k - 4);
    w_lock();
    uint16_t on = W.gw_coil[addr] ? 0 : 1;
    W.gw_coil[addr] = on;                   /* at once; the next poll confirms */
    w_unlock();
    char what[24];
    snprintf(what, sizeof what, k < 4 ? _("relé %d") : _("módulo %d"), k < 4 ? k + 1 : k - 3);
    w_write(5, (uint16_t)addr, 1, &on, what);
}

static void gw_dim_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(U.gw_dim_l[k], "%d %%", v);
    if (c == LV_EVENT_PRESSED) U.gw_drag = true;
    if (c != LV_EVENT_RELEASED) return;
    U.gw_drag = false;
    uint16_t r = (uint16_t)v;
    char what[24];
    snprintf(what, sizeof what, "dimmer %d", k + 1);
    w_write(6, (uint16_t)k, 1, &r, what);
}

static const char *const GW_SENS[10] = { N_("Temperatura"), N_("Humedad"), N_("Presión"), "CO2", N_("Calidad del aire"),
                                         "Wi-Fi", "DS18B20 1", "DS18B20 2", N_("Encendido hace"), N_("Estado") };

static void gw_refresh(void)
{
    if (!U.gw_sens[0]) return;
    w_lock();
    bool ok = W.gw_ok;
    uint16_t in[25], coil[24], hr[4], di[2];
    memcpy(in, W.gw_in, sizeof in);
    memcpy(coil, W.gw_coil, sizeof coil);
    memcpy(hr, W.gw_hr, sizeof hr);
    memcpy(di, W.gw_di, sizeof di);
    w_unlock();
    if (!ok) return;
    char t[32], v[24];
    fmt_num(v, sizeof v, (int16_t)in[0] / 100.0, 1); snprintf(t, sizeof t, "%s °C", v); lv_label_set_text(U.gw_sens[0], t);
    fmt_num(v, sizeof v, in[1] / 100.0, 0); snprintf(t, sizeof t, "%s %%", v); lv_label_set_text(U.gw_sens[1], t);
    if (in[5]) { fmt_num(v, sizeof v, in[5] / 10.0, 1); snprintf(t, sizeof t, "%s hPa", v); } else snprintf(t, sizeof t, "n/d");
    lv_label_set_text(U.gw_sens[2], t);
    if (in[6]) snprintf(t, sizeof t, "%u ppm", in[6]); else snprintf(t, sizeof t, "n/d");
    lv_label_set_text(U.gw_sens[3], t);
    if (in[8] != 0xFFFF) snprintf(t, sizeof t, "%u", in[8]); else snprintf(t, sizeof t, "n/d");
    lv_label_set_text(U.gw_sens[4], t);
    if ((int16_t)in[9]) snprintf(t, sizeof t, "%d dBm", (int16_t)in[9]); else snprintf(t, sizeof t, "%s", _("sin red"));
    lv_label_set_text(U.gw_sens[5], t);
    for (int k = 0; k < 2; k++) {
        if (in[20 + k] == 0x8000) snprintf(t, sizeof t, "%s", _("ausente"));
        else { fmt_num(v, sizeof v, (int16_t)in[20 + k] / 100.0, 2); snprintf(t, sizeof t, "%s °C", v); }
        lv_label_set_text(U.gw_sens[6 + k], t);
    }
    unsigned up = in[3];
    snprintf(t, sizeof t, "%u h %02u min", up / 3600, (up / 60) % 60);
    lv_label_set_text(U.gw_sens[8], t);
    lv_label_set_text(U.gw_sens[9], in[2] ? _("error de sensor") : "OK");
    for (int k = 0; k < 8; k++) {
        bool on = coil[k < 4 ? k : 20 + (k - 4)];
        lv_obj_set_style_bg_color(U.gw_relay[k], on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.gw_relay[k], 0), on ? C_TEAL : AOS_C_DIM, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.gw_relay[k], 1), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
        lv_label_set_text(lv_obj_get_child(U.gw_relay[k], 2), on ? _("Encendido") : _("Apagado"));
    }
    for (int k = 0; k < 4 && !U.gw_drag; k++) {
        lv_slider_set_value(U.gw_dim[k], hr[k], LV_ANIM_ON);
        lv_label_set_text_fmt(U.gw_dim_l[k], "%u %%", hr[k]);
    }
    for (int k = 0; k < 2; k++) {
        lv_label_set_text(U.gw_mag[k], di[k] ? _("Abierto") : _("Cerrado"));
        lv_obj_set_style_text_color(U.gw_mag[k], di[k] ? AOS_C_ORANGE : AOS_C_GREEN, 0);
    }
}

static void build_gw(lv_obj_t *parent, int32_t ch)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = column(parent, w, ch);
    lv_obj_t *head = box(col, w, 88);
    lv_obj_align(aos_label(head, "Gateway DOMCOM", aos_font_large, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, 0);
    connection_card(col, w, false);
    if (!S.gw_host[0]) {
        caption(col, _("Poné la dirección del gateway (el puerto Modbus TCP es 502 salvo que lo hayas cambiado)."), w);
        return;
    }
    int cols = U.land ? 5 : 2;
    int32_t gap = 12, cw = (w - (cols - 1) * gap) / cols;
    section(col, _("SENSORES"));
    lv_obj_t *g = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, gap, 0);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *c = card(g, cw, 104);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_align(aos_label(c, aos_tr(GW_SENS[i]), aos_font_caption, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        U.gw_sens[i] = aos_label(c, "--", aos_font_title, AOS_C_TEXT);
        lv_obj_align(U.gw_sens[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    section(col, _("RELÉS Y MÓDULOS"));
    int rcols = U.land ? 8 : 4;
    int32_t rw = (w - (rcols - 1) * gap) / rcols;
    lv_obj_t *rg = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(rg, gap, 0);
    for (int k = 0; k < 8; k++) {
        lv_obj_t *c = card(rg, rw, 150);
        lv_obj_set_style_pad_all(c, 14, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, gw_relay_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_align(aos_label(c, AOS_SYM_POWER_PLUG, &aos_sym_44, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *n = aos_label(c, "", aos_font_body, AOS_C_TEXT);
        lv_label_set_text_fmt(n, k < 4 ? _("Relé %d") : _("Módulo %d"), k < 4 ? k + 1 : k - 3);
        lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, -26);
        lv_obj_align(aos_label(c, "", aos_font_tiny, AOS_C_DIM), LV_ALIGN_BOTTOM_LEFT, 0, 0);
        for (uint32_t i = 0; i < lv_obj_get_child_count(c); i++) aos_make_decorative(lv_obj_get_child(c, i));
        U.gw_relay[k] = c;
    }
    section(col, _("DIMMERS"));
    lv_obj_t *dc = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dc, 18, 0);
    lv_obj_set_style_pad_row(dc, 22, 0);
    for (int k = 0; k < 4; k++) {
        lv_obj_t *r = box(dc, lv_pct(100), 56);
        lv_obj_t *l = aos_label(r, "", aos_font_body, AOS_C_TEXT);
        lv_label_set_text_fmt(l, "%d", k + 1);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        U.gw_dim[k] = lv_slider_create(r);
        lv_obj_set_size(U.gw_dim[k], w - 36 - 60 - 110, 18);
        lv_slider_set_range(U.gw_dim[k], 0, 100);
        lv_obj_set_style_bg_color(U.gw_dim[k], C_TEAL, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(U.gw_dim[k], 12, LV_PART_KNOB);
        lv_obj_align(U.gw_dim[k], LV_ALIGN_LEFT_MID, 50, 0);
        lv_obj_add_event_cb(U.gw_dim[k], gw_dim_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.gw_dim[k], gw_dim_cb, LV_EVENT_PRESSED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(U.gw_dim[k], gw_dim_cb, LV_EVENT_RELEASED, (void *)(intptr_t)k);
        U.gw_dim_l[k] = aos_label(r, "--", aos_font_body, AOS_C_DIM);
        lv_obj_align(U.gw_dim_l[k], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    section(col, _("SENSORES MAGNÉTICOS"));
    lv_obj_t *mg = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mg, gap, 0);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *c = card(mg, (w - gap) / 2, 96);
        lv_obj_set_style_pad_hor(c, 20, 0);
        lv_obj_t *l = aos_label(c, "", aos_font_body, AOS_C_TEXT);
        lv_label_set_text_fmt(l, _("Puerta %d"), k + 1);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        U.gw_mag[k] = aos_label(c, "--", aos_font_body, AOS_C_DIM);
        lv_obj_align(U.gw_mag[k], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    gw_refresh();
}

/* -------------------------------------------------------------------------- */
/* Explorar                                                                    */
/* -------------------------------------------------------------------------- */

static void ex_fc_cb(lv_event_t *e)
{
    w_lock();
    W.ex_fc = (int)(intptr_t)lv_event_get_user_data(e);
    W.ex_once = true;
    w_unlock();
    settings_save();
    build_page();
}

static void ex_fmt_cb(lv_event_t *e)
{
    S.ex_fmt = (int)(intptr_t)lv_event_get_user_data(e);
    settings_save();
    build_page();
}

static void ex_addr_done(double v) { w_lock(); W.ex_addr = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); W.ex_once = true; w_unlock(); settings_save(); build_page(); }
static void ex_n_done(double v)
{
    int max = W.ex_fc <= 2 ? 2000 : 125;
    w_lock(); W.ex_n = (uint16_t)(v < 1 ? 1 : v > max ? max : v); W.ex_once = true; w_unlock();
    settings_save();
    build_page();
}
static void ex_addr_cb(lv_event_t *e) { keypad(_("Dirección inicial"), W.ex_addr, 0, "", ex_addr_done); }
static void ex_n_cb(lv_event_t *e) { keypad(_("Cantidad"), W.ex_n, 0, "", ex_n_done); }
static void ex_read_cb(lv_event_t *e) { w_lock(); W.ex_once = true; w_unlock(); }

static void ex_live_cb(lv_event_t *e)
{
    w_lock();
    W.ex_live = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    w_unlock();
}

static uint16_t s_ex_wr_addr;

static void ex_write_done(double v)
{
    uint16_t r = (uint16_t)(int32_t)lround(v);
    char what[32];
    snprintf(what, sizeof what, "reg %u", s_ex_wr_addr);
    w_write(6, s_ex_wr_addr, 1, &r, what);
    w_lock(); W.ex_once = true; w_unlock();
}

static void ex_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    w_lock();
    int fc = W.ex_got_fc;
    uint16_t a = (uint16_t)(W.ex_got_addr + i), v = W.ex_val[i];
    w_unlock();
    if (fc == 1) {
        uint16_t on = v ? 0 : 1;
        char what[24];
        snprintf(what, sizeof what, "coil %u", a);
        w_write(5, a, 1, &on, what);
        w_lock(); W.ex_once = true; w_unlock();
    } else if (fc == 3) {
        s_ex_wr_addr = a;
        char t[40];
        snprintf(t, sizeof t, _("Escribir el registro %u"), a);
        keypad(t, v, 0, "", ex_write_done);
    }
}

static void ex_format(int fmt, const uint16_t *v, int i, int n, char *out, size_t len)
{
    switch (fmt) {
    case FMT_S16: snprintf(out, len, "%d", (int16_t)v[i]); break;
    case FMT_HEX: snprintf(out, len, "0x%04X", v[i]); break;
    case FMT_U32: case FMT_S32: case FMT_FLOAT: {
        if (i + 1 >= n) { snprintf(out, len, "·"); break; }
        uint32_t w = (uint32_t)v[i] << 16 | v[i + 1];        /* big-endian word order, the usual */
        if (fmt == FMT_U32) snprintf(out, len, "%lu", (unsigned long)w);
        else if (fmt == FMT_S32) snprintf(out, len, "%ld", (long)(int32_t)w);
        else { float f; memcpy(&f, &w, 4); snprintf(out, len, "%g", (double)f); }
        break;
    }
    default: snprintf(out, len, "%u", v[i]); break;
    }
}

static void ex_refresh(void)
{
    if (!U.ex_list) return;
    w_lock();
    int rc = W.ex_rc, fc = W.ex_got_fc;
    uint16_t a0 = W.ex_got_addr, n = W.ex_got_n, v[125];
    memcpy(v, W.ex_val, sizeof v);
    uint32_t seq = W.ex_seq;
    w_unlock();
    if (U.ex_msg) {
        if (!seq) lv_label_set_text(U.ex_msg, _("Tocá Leer."));
        else if (rc) lv_label_set_text(U.ex_msg, aos_mb_strerror(rc));
        else lv_label_set_text_fmt(U.ex_msg, _("%u valores desde %u"), n, a0);
        lv_obj_set_style_text_color(U.ex_msg, rc ? AOS_C_ORANGE : AOS_C_DIM, 0);
    }
    lv_obj_clean(U.ex_list);
    if (!seq || rc) return;
    bool bits = fc <= 2, wide = !bits && S.ex_fmt >= FMT_U32;
    int32_t w = lv_obj_get_width(U.ex_list);
    int cols = bits ? (U.land ? 8 : 4) : (U.land ? 4 : 2);
    int32_t gap = 10, cw = (w - (cols - 1) * gap) / cols;
    int shown = 0;
    for (int i = 0; i < n && shown < 125; i += wide ? 2 : 1, shown++) {
        lv_obj_t *c = card(U.ex_list, cw, 72);
        lv_obj_set_style_pad_hor(c, 16, 0);
        bool writable = fc == 1 || fc == 3;
        if (writable) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_add_event_cb(c, ex_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *al = lv_label_create(c);
        lv_obj_set_style_text_font(al, &aos_mono_18, 0);
        lv_obj_set_style_text_color(al, C_AMBER, 0);
        lv_label_set_text_fmt(al, "%u", a0 + i);
        lv_obj_align(al, LV_ALIGN_LEFT_MID, 0, 0);
        char t[32];
        if (bits) snprintf(t, sizeof t, "%s", v[i] ? "1" : "0");
        else ex_format(S.ex_fmt, v, i, n, t, sizeof t);
        lv_obj_t *vl = lv_label_create(c);
        lv_obj_set_style_text_font(vl, &aos_mono_22, 0);
        lv_obj_set_style_text_color(vl, bits && v[i] ? AOS_C_GREEN : AOS_C_TEXT, 0);
        lv_label_set_text(vl, t);
        lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void build_exp(lv_obj_t *parent, int32_t ch)
{
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = 520;
        rw = w - lw - AOS_UI_PAD;
        left = column(parent, lw, ch);
        right = column(parent, rw, ch);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(parent, w, ch);
    }
    lv_obj_t *head = box(left, lw, 88);
    lv_obj_align(aos_label(head, _("Explorar"), aos_font_large, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, 0);
    connection_card(left, lw, true);
    section(left, _("PEDIDO"));
    lv_obj_t *c = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_t *r = chips_row(c, lw - 36);
    for (int fc = 1; fc <= 4; fc++) chip(r, aos_tr(FC_NAME[fc]), W.ex_fc == fc, ex_fc_cb, (void *)(intptr_t)fc);
    lv_obj_t *r2 = chips_row(c, lw - 36);
    char t[32];
    snprintf(t, sizeof t, "%s %u", _("Desde"), W.ex_addr);
    chip(r2, t, false, ex_addr_cb, NULL);
    snprintf(t, sizeof t, "%s %u", _("Cantidad"), W.ex_n);
    chip(r2, t, false, ex_n_cb, NULL);
    if (W.ex_fc >= 3) {
        lv_obj_t *r3 = chips_row(c, lw - 36);
        for (int f = 0; f < FMT_COUNT; f++) chip(r3, FMT_NAME[f], S.ex_fmt == f, ex_fmt_cb, (void *)(intptr_t)f);
    }
    lv_obj_t *r4 = box(c, lw - 36, 80);
    lv_obj_set_flex_flow(r4, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r4, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r4, 16, 0);
    pill(r4, AOS_SYM_RESTART, _("Leer"), C_TEAL, ex_read_cb, NULL);
    aos_label(r4, _("En vivo"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(r4);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, C_TEAL, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, W.ex_live);
    lv_obj_add_event_cb(sw, ex_live_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (W.ex_fc == 1 || W.ex_fc == 3) caption(left, W.ex_fc == 1 ? _("Tocá una coil para invertirla.") : _("Tocá un registro para escribirlo (FC 06)."), lw);
    section(right, _("RESPUESTA"));
    U.ex_msg = aos_label(right, "", aos_font_small, AOS_C_DIM);
    U.ex_list = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.ex_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(U.ex_list, 10, 0);
    ex_refresh();
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    kp_close();
    lv_obj_clean(U.content);
    U.status = U.rd_v = U.rd_chart = U.ex_list = U.ex_msg = NULL;
    memset(U.gw_sens, 0, sizeof U.gw_sens);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_TEAL : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    aos_mb_link_t l = link_for_tab();
    bool no_host = l.transport == AOS_MB_TCP && !l.host[0];      /* nothing to connect to yet */
    w_plan(no_host ? PLAN_NONE : S.tab == TAB_RIDEN ? PLAN_RIDEN : S.tab == TAB_GW ? PLAN_GW : PLAN_EXP, &l);
    int32_t ch = lv_obj_get_height(U.content);
    if (S.tab == TAB_RIDEN) build_riden(U.content, ch);
    else if (S.tab == TAB_GW) build_gw(U.content, ch);
    else build_exp(U.content, ch);
}

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    settings_save();
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    static uint32_t n;
    n++;
    /* keep the plan alive; it lapses two seconds after the app goes */
    w_lock();
    W.plan_ms = (uint32_t)aos_hal_uptime_ms();
    uint32_t seq = W.seq, wseq = W.wseq, exseq = W.ex_seq;
    char wmsg[96];
    bool wbad = W.wbad;
    snprintf(wmsg, sizeof wmsg, "%s", W.wmsg);
    w_unlock();
    if (wseq != U.wseen) {
        U.wseen = wseq;
        if (wbad && wmsg[0]) aos_ui_toast(wmsg, 3000);
    }
    if (seq != U.seen) {
        U.seen = seq;
        status_refresh();
        if (S.tab == TAB_RIDEN) rd_refresh();
        else if (S.tab == TAB_GW) gw_refresh();
    }
    if (exseq != U.exseen && S.tab == TAB_EXP) { U.exseen = exseq; ex_refresh(); }
    if (S.tab == TAB_RIDEN && n % 2 == 0) {
        rd_chart_push();
        w_lock();
        double p = W.rd_fast_ok ? ((uint32_t)W.rd_fast[8] << 16 | W.rd_fast[9]) / 100.0 : NAN;
        w_unlock();
        if (!isnan(p)) {
            if (S.p_n < 120) S.p_hist[S.p_n++] = (float)p;
            else { memmove(S.p_hist, S.p_hist + 1, sizeof S.p_hist - sizeof S.p_hist[0]); S.p_hist[119] = (float)p; }
        }
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    w_start();
    settings_load();
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.wseen = W.wseq;
    const int32_t tab_h = U.land ? 96 : 116;
    U.content = box(root, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 8, 0);
    lv_obj_update_layout(U.content);
    lv_obj_t *bar = box(root, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = box(bar, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM), LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_align(aos_label(t, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    build_page();
    U.timer = lv_timer_create(timer_cb, 125, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { kp_close(); return true; }
    return false;
}

void aos_app_modbus_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.modbus", .name = "Modbus", .icon = AOS_SYM_LAN,
            .color_a = 0x10B981, .color_b = 0x047857,
            .order = 520,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
