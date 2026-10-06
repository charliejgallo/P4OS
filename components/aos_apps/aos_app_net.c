/*
 * P4OS - Red: the network tools of the bench, over aos_nettools.c.
 *
 * Four tabs:
 *
 *   Ping    any host or address: a ping a second with the latency as bars,
 *           min/avg/max, loss and jitter, and what the name resolved to.
 *           ICMP where the platform lets us; otherwise the time a TCP
 *           connect to port 80 takes, and the screen says so.
 *   Hosts   the /24 the board is on: who answers the ping, which of the ports
 *           that matter in a house are open (HA, MQTT, Modbus, ESPHome, the
 *           Rigol, RTSP...), what each one probably is, and its name from
 *           mDNS. A tap on one: all its ports from 1 to 1024 plus the usual
 *           high ones, and a button to ping it.
 *   WiFi    the networks around with their channel, width and security, a
 *           graph of the channels with every access point as a bell, and the
 *           signal of the one we are on, live.
 *   mDNS    who announces what: Home Assistant, ESPHome, MQTT, web pages,
 *           HomeKit, printers...
 *
 * Nothing here touches the network: the service's threads do, and the
 * screen reads what they leave from an lv_timer. Leaving the app stops them.
 */
#include "aos_apps.h"
#include "aos_nettools.h"
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

#define C_NET     lv_color_hex(0x38BDF8)
#define C_NET_D   lv_color_hex(0x0369A1)
#define C_KEYTOP  lv_color_hex(0xF2F2F7)

enum { TAB_PING, TAB_HOSTS, TAB_WIFI, TAB_MDNS, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { "Ping", "Hosts", "WiFi", "mDNS" };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_PULSE, AOS_SYM_LAN_PENDING, AOS_SYM_WIFI, AOS_SYM_DNS };

#define RECENT_MAX 6

/* -------------------------------------------------------------------------- */
/* State kept across rotations, and the settings                               */
/* -------------------------------------------------------------------------- */

static struct {
    bool loaded;
    int tab;
    char host[64];
    char recent[RECENT_MAX][64];
    int nrecent;
    bool full;
    uint32_t sel;               /* the host open in Hosts, 0 none */
    int mfilter;                /* -1 all, else an NT_MDNS_TYPES index */
} S = { .mfilter = -1 };

static void settings_load(void)
{
    if (S.loaded) return;
    S.loaded = true;
    int32_t v;
    if (aos_hal_pref_get_i32("net_tab", &v) && v >= 0 && v < TAB_COUNT) S.tab = v;
    if (aos_hal_pref_get_i32("net_full", &v)) S.full = v != 0;
    if (!aos_hal_pref_get_str("net_host", S.host, sizeof S.host)) S.host[0] = 0;
    char r[RECENT_MAX * 64 + 8];
    if (aos_hal_pref_get_str("net_recent", r, sizeof r)) {
        char *save = NULL;
        for (char *t = strtok_r(r, ",", &save); t && S.nrecent < RECENT_MAX; t = strtok_r(NULL, ",", &save))
            if (*t) snprintf(S.recent[S.nrecent++], sizeof S.recent[0], "%s", t);
    }
}

static void settings_save(void)
{
    aos_hal_pref_set_i32("net_tab", S.tab);
    aos_hal_pref_set_i32("net_full", S.full);
    aos_hal_pref_set_str("net_host", S.host);
    char r[RECENT_MAX * 64 + 8] = "";
    for (int i = 0; i < S.nrecent; i++) {
        if (i) strcat(r, ",");
        strcat(r, S.recent[i]);
    }
    aos_hal_pref_set_str("net_recent", r);
}

static void recent_push(const char *h)
{
    int at = -1;
    for (int i = 0; i < S.nrecent; i++) if (!strcmp(S.recent[i], h)) at = i;
    if (at < 0) at = S.nrecent < RECENT_MAX ? S.nrecent++ : RECENT_MAX - 1;
    for (int i = at; i > 0; i--) memcpy(S.recent[i], S.recent[i - 1], sizeof S.recent[0]);
    snprintf(S.recent[0], sizeof S.recent[0], "%s", h);
}

/* -------------------------------------------------------------------------- */
/* The screen                                                                  */
/* -------------------------------------------------------------------------- */

/* A graph drawn once, into a picture of its own in PSRAM, whenever its
 * data change. Drawn live in LV_EVENT_DRAW_MAIN, the channel graphs (a
 * curve of 30-60 antialiased segments, a fill and a name per network) were
 * drawn again in every frame of a scroll and in every band of the draw
 * buffer: thousands of lines a frame with a few dozen networks around, and
 * the page crawled until they were scrolled out of view. Now a scroll only
 * copies pixels. */
typedef void (*paint_fn)(lv_layer_t *layer, const lv_area_t *c, bool five);

typedef struct {
    lv_obj_t     *holder;   /* the card or box it covers, edge to edge */
    lv_obj_t     *canvas;   /* made on the first paint */
    void         *px;
    int32_t       w, h;
    lv_color_format_t cf;   /* RGB565 over a known background, ARGB8888 over rounded corners */
    lv_color_t    bg;
    paint_fn      paint;
    bool          five;
} graph_t;

typedef struct {
    /* Ping */
    lv_obj_t *p_host, *p_res, *p_btn, *p_big, *p_badge, *p_sub, *p_fact[6], *p_chart, *p_err;
    uint32_t p_seen;
    int p_state;
    /* Hosts */
    lv_obj_t *h_cap, *h_btn, *h_bar, *h_state, *h_list, *h_right;
    int32_t h_rw;
    uint32_t h_seen, h_sig, h_built_ms;
    int32_t h_list_w, w_list_w, m_list_w;
    lv_obj_t *d_btn, *d_bar, *d_state, *d_list, *d_title, *d_kind, *d_known, *d_rtt;
    uint32_t d_seen;
    int d_nopen;
    /* WiFi */
    lv_obj_t *w_l5, *w_cap, *w_ssid, *w_rssi, *w_q, *w_info, *w_list, *w_btn, *w_when, *w_bars;
    graph_t w_trace, w_g24, w_g5;
    uint32_t w_seen, w_scanned;
    /* mDNS */
    lv_obj_t *m_btn, *m_state, *m_list, *m_filters;
    uint32_t m_seen;
} page_t;

AOS_BSS_PSRAM static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay, *ta, *kb;
    lv_timer_t *timer;
    int32_t W, H, cw, ch;
    bool land;
    page_t pg;
    /* copies the charts draw from, refreshed when the service's seq moves */
    float p_hist[NT_PING_HIST];
    int p_n;
    float p_avg;
    aos_wifi_ap_ex_t w_aps[NT_MAX_APS];
    int w_n;
    int8_t w_rssi[NT_RSSI_HIST];
    int w_rn;
    aos_wifi_ap_ex_t w_cur;
    bool w_cur_ok;
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

static lv_obj_t *vcard(lv_obj_t *parent, int32_t w, int32_t pad, int32_t gap)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, pad, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, gap, 0);
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

static lv_obj_t *row(lv_obj_t *parent, int32_t w, int32_t h, int32_t gap)
{
    lv_obj_t *r = box(parent, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    aos_make_decorative(aos_label(b, glyph ? glyph : "", &aos_sym_28, lv_color_white()));
    aos_make_decorative(aos_label(b, text ? text : "", aos_font_body, lv_color_white()));
    return b;
}

static void pill_set(lv_obj_t *b, const char *glyph, const char *text, lv_color_t bg)
{
    if (!b) return;
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_label_set_text(lv_obj_get_child(b, 0), glyph);
    lv_label_set_text(lv_obj_get_child(b, 1), text);
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? C_KEYTOP : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_center(aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    return c;
}

/* The small tag a port wears in a list: "8123 Home Assistant". */
static lv_obj_t *port_tag(lv_obj_t *parent, uint16_t port, bool big)
{
    const char *n = nt_port_name(port);
    char t[40];
    if (n[0]) snprintf(t, sizeof t, "%u %s", port, aos_tr(n));
    else snprintf(t, sizeof t, "%u", port);
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, big ? 52 : 40);
    lv_obj_set_style_radius(c, big ? 26 : 20, 0);
    lv_obj_set_style_pad_hor(c, big ? 18 : 14, 0);
    bool hot = port == 8123 || port == 1883 || port == 502 || port == 6053 || port == 5555 || port == 554;
    lv_obj_set_style_bg_color(c, hot ? C_NET_D : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_center(aos_label(c, t, big ? aos_font_small : aos_font_tiny, AOS_C_TEXT));
    aos_make_decorative(c);
    return c;
}

static lv_obj_t *wrap_row(lv_obj_t *parent, int32_t w, int32_t gap)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, gap, 0);
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

static lv_obj_t *header(lv_obj_t *parent, int32_t w, const char *title, lv_obj_t **cap)
{
    lv_obj_t *head = box(parent, w, 88);
    lv_obj_align(aos_label(head, title, aos_font_large, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, -12);
    lv_obj_t *c = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(c, w - 8);
    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    if (cap) *cap = c;
    return head;
}

static lv_obj_t *progress(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, 10);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_NET, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_radius(b, 5, LV_PART_INDICATOR);
    lv_bar_set_range(b, 0, 1000);
    return b;
}

static void bar_set(lv_obj_t *b, int done, int total)
{
    if (!b) return;
    lv_bar_set_value(b, total > 0 ? (int32_t)((int64_t)done * 1000 / total) : 0, LV_ANIM_OFF);
}

/* Two columns in landscape, one in portrait. */
static void split(lv_obj_t *parent, int32_t lw_land, lv_obj_t **left, lv_obj_t **right, int32_t *lw, int32_t *rw)
{
    int32_t w = U.cw;
    if (U.land) {
        *lw = lw_land;
        *rw = w - lw_land - AOS_UI_PAD;
        *left = column(parent, *lw, U.ch);
        *right = column(parent, *rw, U.ch);
        lv_obj_set_x(*right, *lw + AOS_UI_PAD);
    } else {
        *lw = *rw = w;
        *left = *right = column(parent, w, U.ch);
    }
}

/* 12.34 -> "12,34" */
static void fmt_num(char *out, size_t n, double v, int dec)
{
    snprintf(out, n, "%.*f", dec, v);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

static void fmt_ms(char *out, size_t n, float ms)
{
    if (ms < 0) { snprintf(out, n, "--"); return; }
    fmt_num(out, n, ms, ms < 10 ? 2 : ms < 100 ? 1 : 0);
}

static void fmt_elapsed(char *out, size_t n, uint32_t ms)
{
    if (ms < 1000) snprintf(out, n, "%u ms", (unsigned)ms);
    else if (ms < 10000) { fmt_num(out, n, ms / 1000.0, 1); strncat(out, " s", n - strlen(out) - 1); }
    else if (ms < 120000) snprintf(out, n, "%u s", (unsigned)(ms / 1000));
    else snprintf(out, n, "%u min %u s", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60));
}

/* ---- the text field (a host) ---- */

static void (*s_text_done)(const char *);

static void kp_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = U.ta = U.kb = NULL;
}

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
    lv_textarea_set_max_length(U.ta, 63);
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
    lv_obj_t *hint = caption(U.overlay, _("Un nombre (homeassistant.local, google.com) o una dirección (192.168.1.10)."), U.W - 2 * AOS_UI_PAD);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 204);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_cb, LV_EVENT_ALL, NULL);
}

/* -------------------------------------------------------------------------- */
/* Ping                                                                        */
/* -------------------------------------------------------------------------- */

static void ping_go(const char *host)
{
    snprintf(S.host, sizeof S.host, "%s", host);
    recent_push(host);
    settings_save();
    nt_ping_start(host);
}

static void host_done(const char *h)
{
    while (*h == ' ') h++;
    char t[64];
    snprintf(t, sizeof t, "%s", h);
    for (int n = (int)strlen(t); n > 0 && t[n - 1] == ' '; n--) t[n - 1] = 0;
    if (!t[0]) return;
    ping_go(t);
    build_page();
}

static void host_cb(lv_event_t *e) { text_entry(_("Equipo a medir"), S.host, host_done); }

static void ping_btn_cb(lv_event_t *e)
{
    nt_lock();
    bool running = nt_ping()->state == NT_BUSY;
    nt_unlock();
    if (running) nt_ping_stop();
    else if (S.host[0]) ping_go(S.host);
    else text_entry(_("Equipo a medir"), "", host_done);
}

static void recent_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nrecent) return;
    char h[64];
    snprintf(h, sizeof h, "%s", S.recent[i]);
    ping_go(h);
    build_page();
}

static int32_t nice_ceil(float v)
{
    static const int32_t STEPS[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000 };
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) if (v <= STEPS[i]) return STEPS[i];
    return 10000;
}

static void draw_text(lv_layer_t *layer, const char *t, const lv_font_t *f, lv_color_t c,
                      int32_t x1, int32_t y1, int32_t x2, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;
    d.font = f;
    d.color = c;
    d.align = al;
    lv_area_t a = { x1, y1, x2, y1 + lv_font_get_line_height(f) };
    lv_draw_label(layer, &d, &a);
}

static void draw_hline(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y, lv_color_t c, int32_t w, bool dash)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y;
    d.p2.x = x2; d.p2.y = y;
    d.color = c;
    d.width = w;
    if (dash) { d.dash_width = 8; d.dash_gap = 6; }
    lv_draw_line(layer, &d);
}

static void fill_rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa, int32_t r)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    d.radius = r;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static lv_color_t lat_color(float ms)
{
    return ms < 20 ? AOS_C_GREEN : ms < 80 ? C_NET : ms < 200 ? AOS_C_YELLOW : AOS_C_ORANGE;
}

static void ping_chart_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const lv_font_t *f = aos_font_tiny;
    int32_t lh = lv_font_get_line_height(f);
    int32_t x0 = c.x1 + 84, x1 = c.x2 - 20, y0 = c.y1 + 20, y1 = c.y2 - 20 - lh;
    float mx = 0;
    for (int i = 0; i < U.p_n; i++) if (U.p_hist[i] > mx) mx = U.p_hist[i];
    int32_t top = nice_ceil(mx * 1.15f > 1 ? mx * 1.15f : 1);
    char t[24];
    for (int k = 0; k <= 4; k++) {
        int32_t y = y1 - (y1 - y0) * k / 4;
        draw_hline(layer, x0, x1, y, AOS_C_CARD2, 2, false);
        float v = top * k / 4.0f;
        fmt_num(t, sizeof t, v, top < 10 ? 2 : top <= 50 ? 1 : 0);
        for (size_t n = strlen(t); strchr(t, ',') && n && (t[n - 1] == '0' || t[n - 1] == ','); n--) {
            bool comma = t[n - 1] == ',';
            t[n - 1] = 0;
            if (comma) break;
        }
        strcat(t, " ms");
        draw_text(layer, t, f, AOS_C_DIM, c.x1 + 8, y - lh / 2, x0 - 10, LV_TEXT_ALIGN_RIGHT);
    }
    draw_text(layer, _("hace 2 min"), f, AOS_C_DIM, x0, y1 + 8, x0 + 300, LV_TEXT_ALIGN_LEFT);
    draw_text(layer, _("ahora"), f, AOS_C_DIM, x1 - 200, y1 + 8, x1, LV_TEXT_ALIGN_RIGHT);
    if (!U.p_n) {
        draw_text(layer, _("Tocá Empezar para medir"), aos_font_small, AOS_C_DIM, x0, (y0 + y1) / 2 - 14, x1, LV_TEXT_ALIGN_CENTER);
        return;
    }
    float slot = (float)(x1 - x0) / NT_PING_HIST;
    int off = NT_PING_HIST - U.p_n;
    for (int i = 0; i < U.p_n; i++) {
        int32_t bx1 = x0 + (int32_t)((off + i) * slot) + 1;
        int32_t bx2 = x0 + (int32_t)((off + i + 1) * slot) - 1;
        if (bx2 < bx1) bx2 = bx1;
        float v = U.p_hist[i];
        if (v < 0) {
            fill_rect(layer, bx1, y0, bx2, y1, AOS_C_RED, LV_OPA_30, 0);
            fill_rect(layer, bx1, y0, bx2, y0 + 10, AOS_C_RED, LV_OPA_COVER, 0);
            continue;
        }
        int32_t h = (int32_t)((y1 - y0) * (v / top));
        if (h < 3) h = 3;
        fill_rect(layer, bx1, y1 - h, bx2, y1, lat_color(v), LV_OPA_COVER, slot > 5 ? 2 : 0);
    }
    if (U.p_avg > 0) {
        int32_t y = y1 - (int32_t)((y1 - y0) * (U.p_avg / top));
        draw_hline(layer, x0, x1, y, lv_color_white(), 2, true);
    }
}

static void ping_refresh(void)
{
    page_t *g = &U.pg;
    if (!g->p_big) return;
    nt_ping_t p;
    nt_lock();
    const nt_ping_t *s = nt_ping();
    uint32_t seq = s->seq;
    if (seq == g->p_seen) { nt_unlock(); return; }
    p = *s;
    nt_unlock();
    g->p_seen = seq;
    memcpy(U.p_hist, p.hist, sizeof U.p_hist);
    U.p_n = p.hist_n;
    U.p_avg = p.recv ? p.avg_ms : 0;

    bool running = p.state == NT_BUSY;
    if (p.state == NT_FAILED && g->p_state != NT_FAILED) {
        /* a name that does not resolve does not stay among the recent ones */
        for (int i = 0; i < S.nrecent; i++) {
            if (strcmp(S.recent[i], p.host)) continue;
            memmove(S.recent[i], S.recent[i + 1], (size_t)(S.nrecent - i - 1) * sizeof S.recent[0]);
            S.nrecent--;
            settings_save();
            break;
        }
    }
    if (running != (g->p_state == NT_BUSY)) {
        pill_set(g->p_btn, running ? AOS_SYM_STOP : AOS_SYM_PLAY, running ? _("Detener") : _("Empezar"),
                 running ? AOS_C_RED : AOS_C_GREEN);
    }
    g->p_state = p.state;
    lv_label_set_text(g->p_host, S.host[0] ? S.host : _("Tocá para elegir un equipo"));
    lv_obj_set_style_text_color(g->p_host, S.host[0] ? AOS_C_TEXT : AOS_C_DIM, 0);

    char t[160], a[24];
    if (p.state == NT_FAILED) {
        snprintf(t, sizeof t, "%s: %s", aos_tr(p.err), p.host);
        lv_label_set_text(g->p_res, t);
        lv_obj_set_style_text_color(g->p_res, AOS_C_ORANGE, 0);
    } else if (p.resolved && strcmp(p.host, S.host) == 0) {
        bool literal = strcmp(p.ip, p.host) == 0;
        if (literal) snprintf(t, sizeof t, "%s", p.ip);
        else snprintf(t, sizeof t, _("%s · DNS en %u ms"), p.ip, (unsigned)p.dns_ms);
        if (!p.icmp) {
            size_t n = strlen(t);
            snprintf(t + n, sizeof t - n, "  ·  %s", _("sin ICMP: mide conectar por TCP al puerto 80"));
        }
        lv_label_set_text(g->p_res, t);
        lv_obj_set_style_text_color(g->p_res, p.icmp ? AOS_C_DIM : AOS_C_ORANGE, 0);
    } else if (running) {
        lv_label_set_text(g->p_res, _("buscando la dirección…"));
        lv_obj_set_style_text_color(g->p_res, AOS_C_DIM, 0);
    } else {
        char me[16];
        snprintf(me, sizeof me, "%s", aos_hal_net_ip());
        snprintf(t, sizeof t, _("Desde %s, un ping por segundo"), me[0] ? me : "?");
        lv_label_set_text(g->p_res, t);
        lv_obj_set_style_text_color(g->p_res, AOS_C_DIM, 0);
    }

    float last = p.hist_n ? p.hist[p.hist_n - 1] : -1;
    fmt_ms(a, sizeof a, p.sent ? last : -1);
    lv_label_set_text(g->p_big, a);
    lv_obj_set_style_text_color(g->p_big, !p.sent ? AOS_C_DIM : last < 0 ? AOS_C_RED : lat_color(last), 0);
    lv_label_set_text(g->p_badge, p.sent || running ? (p.icmp || !p.resolved ? "ICMP" : "TCP :80") : "");
    lv_obj_set_style_bg_opa(g->p_badge, p.sent || running ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(g->p_badge, p.icmp || !p.resolved ? C_NET_D : AOS_C_ORANGE, 0);
    if (!p.sent) lv_label_set_text(g->p_sub, running ? _("esperando la primera respuesta") : _("sin medir"));
    else if (last < 0) lv_label_set_text_fmt(g->p_sub, _("la última se perdió · %u enviados"), (unsigned)p.sent);
    else if (p.ttl) lv_label_set_text_fmt(g->p_sub, _("TTL %d · %u enviados"), p.ttl, (unsigned)p.sent);
    else lv_label_set_text_fmt(g->p_sub, _("%u enviados"), (unsigned)p.sent);

    float vals[4] = { p.recv ? p.min_ms : -1, p.recv ? p.avg_ms : -1, p.recv ? p.max_ms : -1, p.recv > 1 ? p.jitter_ms : -1 };
    for (int i = 0; i < 4; i++) {
        if (!g->p_fact[i]) continue;
        fmt_ms(a, sizeof a, vals[i]);
        lv_label_set_text_fmt(g->p_fact[i], "%s ms", a);
    }
    if (p.sent) {
        fmt_num(a, sizeof a, 100.0 * (p.sent - p.recv) / p.sent, p.sent - p.recv && p.sent > 100 ? 1 : 0);
        lv_label_set_text_fmt(g->p_fact[4], "%s %%", a);
        lv_obj_set_style_text_color(g->p_fact[4], p.recv == p.sent ? AOS_C_TEXT : AOS_C_ORANGE, 0);
    } else {
        lv_label_set_text(g->p_fact[4], "-- %");
    }
    lv_label_set_text_fmt(g->p_fact[5], "%u / %u", (unsigned)p.recv, (unsigned)p.sent);
    lv_obj_invalidate(g->p_chart);
}

static void build_ping(void)
{
    page_t *g = &U.pg;
    lv_obj_t *left, *right;
    int32_t lw, rw;
    split(U.content, 560, &left, &right, &lw, &rw);
    lv_obj_t *cap;
    header(left, lw, "Ping", &cap);
    lv_label_set_text_fmt(cap, _("Latencia y pérdida hacia un equipo · estás en %s"), aos_hal_net_ip()[0] ? aos_hal_net_ip() : "?");

    lv_obj_t *hc = vcard(left, lw, 22, 12);
    lv_obj_t *hr = box(hc, lv_pct(100), 64);
    lv_obj_add_flag(hr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hr, host_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = aos_label(hr, AOS_SYM_IP_NETWORK_OUTLINE, &aos_sym_44, C_NET);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    g->p_host = aos_label(hr, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_size(g->p_host, lw - 44 - 150, lv_font_get_line_height(aos_font_title));
    lv_label_set_long_mode(g->p_host, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(g->p_host, LV_ALIGN_LEFT_MID, 62, 0);
    lv_obj_align(aos_label(hr, AOS_SYM_PENCIL, &aos_sym_28, AOS_C_DIM), LV_ALIGN_RIGHT_MID, 0, 0);
    aos_make_decorative(ic);
    lv_obj_t *br = box(hc, lw - 44, 80);
    g->p_res = aos_label(br, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(g->p_res, lw - 44 - 240);
    lv_label_set_long_mode(g->p_res, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(g->p_res, LV_ALIGN_LEFT_MID, 0, 0);
    g->p_btn = pill(br, AOS_SYM_PLAY, _("Empezar"), AOS_C_GREEN, ping_btn_cb, NULL);
    lv_obj_align(g->p_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    g->p_state = NT_IDLE;
    nt_lock();
    bool running = nt_ping()->state == NT_BUSY;
    nt_unlock();
    if (running) { pill_set(g->p_btn, AOS_SYM_STOP, _("Detener"), AOS_C_RED); g->p_state = NT_BUSY; }

    if (S.nrecent) {
        section(left, _("RECIENTES"));
        lv_obj_t *rr = wrap_row(left, lw, 10);
        for (int i = 0; i < S.nrecent; i++) chip(rr, S.recent[i], !strcmp(S.recent[i], S.host), recent_cb, (void *)(intptr_t)i);
    }

    /* the last answer, big, with min/avg/max beside it */
    lv_obj_t *big = vcard(right, rw, 22, 4);
    lv_obj_t *mm = box(big, 230, LV_SIZE_CONTENT);
    lv_obj_add_flag(mm, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(mm, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(mm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mm, 6, 0);
    static const char *const MM[3] = { N_("mínimo"), N_("promedio"), N_("máximo") };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = box(mm, 230, LV_SIZE_CONTENT);
        lv_obj_align(aos_label(r, aos_tr(MM[i]), aos_font_caption, AOS_C_DIM), LV_ALIGN_LEFT_MID, 0, 0);
        g->p_fact[i] = aos_label(r, "--", aos_font_small, AOS_C_TEXT);
        lv_obj_align(g->p_fact[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_t *vr = box(big, rw - 44 - 240, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vr, 12, 0);
    g->p_big = aos_label(vr, "--", &aos_inter_num_96, AOS_C_DIM);
    lv_obj_t *unit = aos_label(vr, "ms", aos_font_title, AOS_C_DIM);
    lv_obj_set_style_margin_bottom(unit, 14, 0);
    g->p_badge = aos_label(vr, "", aos_font_caption, lv_color_white());
    lv_obj_set_style_radius(g->p_badge, 12, 0);
    lv_obj_set_style_pad_hor(g->p_badge, 12, 0);
    lv_obj_set_style_pad_ver(g->p_badge, 4, 0);
    lv_obj_set_style_margin_bottom(g->p_badge, 22, 0);
    lv_obj_set_style_margin_left(g->p_badge, 8, 0);
    g->p_sub = aos_label(big, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(g->p_sub, rw - 44 - 240);
    lv_label_set_long_mode(g->p_sub, LV_LABEL_LONG_MODE_WRAP);

    section(right, _("LATENCIA, UNA BARRA POR PING"));
    g->p_chart = card(right, rw, U.land ? U.ch - 250 : 270);
    lv_obj_add_event_cb(g->p_chart, ping_chart_draw, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *fg = box(U.land ? left : right, U.land ? lw : rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(fg, 12, 0);
    static const char *const FACT[6] = { "", "", "", "Jitter", N_("Pérdida"), N_("Respondidos") };
    int32_t fwid = U.land ? lw : rw;
    int32_t fw = (fwid - 2 * 12) / 3;
    for (int i = 3; i < 6; i++) {
        lv_obj_t *c = card(fg, fw, 104);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_align(aos_label(c, aos_tr(FACT[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        g->p_fact[i] = aos_label(c, "--", aos_font_body, AOS_C_TEXT);
        lv_obj_align(g->p_fact[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    caption(U.land ? left : right, _("El jitter es cuánto cambia la latencia de un ping al siguiente, en promedio. La línea punteada del gráfico es el promedio."), U.land ? lw : rw);
    g->p_seen = 0xFFFFFFFFu;
    ping_refresh();
}

/* -------------------------------------------------------------------------- */
/* Hosts                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { const char *kind; const char *glyph; uint32_t color; } kind_look_t;

static void kind_look(const nt_host_t *h, const char **glyph, lv_color_t *col)
{
    static const kind_look_t L[] = {
        { "Este P4OS",             AOS_SYM_MONITOR,          0x38BDF8 },
        { "Home Assistant",        AOS_SYM_HOME_ASSISTANT,   0x41BDF5 },
        { "ESPHome",               AOS_SYM_CHIP,             0x94A3B8 },
        { "Rigol (SCPI)",          AOS_SYM_SINE_WAVE,        0xEF4444 },
        { "Cámara RTSP",           AOS_SYM_CCTV,             0x64748B },
        { "Modbus TCP",            AOS_SYM_LAN,              0x10B981 },
        { "Broker MQTT",           AOS_SYM_ACCESS_POINT,     0xA78BFA },
        { "Chromecast",            AOS_SYM_TELEVISION,       0xF59E0B },
        { "Impresora",             AOS_SYM_FILE_DOCUMENT_OUTLINE, 0x60A5FA },
        { "HomeKit",               AOS_SYM_LIGHTBULB,        0xFBBF24 },
        { "AirPlay",               AOS_SYM_SPEAKER,          0xF472B6 },
        { "Carpetas compartidas",  AOS_SYM_FOLDER,           0x60A5FA },
        { "Servidor Linux",        AOS_SYM_SERVER_NETWORK,   0x22C55E },
        { "Equipo con SSH",        AOS_SYM_SERVER_NETWORK,   0x22C55E },
        { "Equipo con página web", AOS_SYM_WEB,              0x38BDF8 },
    };
    const char *k = nt_guess(h);
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        if (!strcmp(L[i].kind, k)) { *glyph = L[i].glyph; *col = lv_color_hex(L[i].color); return; }
    }
    *glyph = AOS_SYM_IP_NETWORK_OUTLINE;
    *col = lv_color_hex(0x8E8E93);
}

static lv_obj_t *round_icon(lv_obj_t *parent, const char *glyph, lv_color_t col, int32_t size, const lv_font_t *f)
{
    lv_obj_t *c = box(parent, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, col, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
    lv_obj_center(aos_label(c, glyph, f, col));
    aos_make_decorative(c);
    return c;
}

static bool find_host(uint32_t ip, nt_host_t *out)
{
    bool ok = false;
    nt_lock();
    const nt_scan_t *s = nt_scan();
    for (int i = 0; i < s->n; i++) if (s->hosts[i].ip == ip) { *out = s->hosts[i]; ok = true; break; }
    nt_unlock();
    return ok;
}

static void hosts_scan_cb(lv_event_t *e)
{
    nt_lock();
    bool busy = nt_scan()->state == NT_BUSY;
    nt_unlock();
    if (busy) nt_scan_stop();
    else nt_scan_start(S.full);
    U.pg.h_seen = 0xFFFFFFFFu;
}

static void mode_cb(lv_event_t *e)
{
    S.full = (int)(intptr_t)lv_event_get_user_data(e) != 0;
    settings_save();
    build_page();
}

static void build_detail(lv_obj_t *parent, int32_t w);

static void host_open_cb(lv_event_t *e)
{
    S.sel = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    page_t *g = &U.pg;
    if (U.land && g->h_right) {
        /* only the right half changes: the list keeps its scroll */
        lv_obj_clean(g->h_right);
        g->d_btn = g->d_bar = g->d_state = g->d_list = NULL;
        build_detail(g->h_right, g->h_rw);
        lv_obj_scroll_to_y(g->h_right, 0, LV_ANIM_OFF);
        /* the ring moves to the row tapped (not a rebuild: we are inside
         * that row's own event) */
        for (uint32_t i = 0; g->h_list && i < lv_obj_get_child_count(g->h_list); i++) {
            lv_obj_t *r = lv_obj_get_child(g->h_list, (int32_t)i);
            bool on = (uint32_t)(uintptr_t)lv_obj_get_user_data(r) == S.sel;
            lv_obj_set_style_border_color(r, C_NET, 0);
            lv_obj_set_style_border_width(r, on ? 3 : 0, 0);
        }
        return;
    }
    build_page();
}

static void detail_close_cb(lv_event_t *e)
{
    S.sel = 0;
    build_page();
}

static void detail_ports_cb(lv_event_t *e)
{
    nt_lock();
    const nt_ports_t *r = nt_ports();
    bool busy = r->state == NT_BUSY, mine = r->ip == S.sel;
    nt_unlock();
    if (busy && mine) nt_ports_stop();
    else if (!busy) nt_ports_start(S.sel);
    else aos_ui_toast(_("Ya hay otro escaneo de puertos en marcha"), 2000);
    U.pg.d_seen = 0xFFFFFFFFu;
}

static void detail_ping_cb(lv_event_t *e)
{
    char ip[16];
    nt_ip_str(S.sel, ip, sizeof ip);
    S.tab = TAB_PING;
    ping_go(ip);
    build_page();
}

static void host_row(lv_obj_t *list, const nt_host_t *h, int32_t w)
{
    lv_obj_t *r = card(list, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(r, 16, 0);
    lv_obj_set_style_pad_right(r, 44, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    if (h->ip == S.sel) {
        lv_obj_set_style_border_color(r, C_NET, 0);
        lv_obj_set_style_border_width(r, 3, 0);
    }
    lv_obj_add_event_cb(r, host_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)h->ip);
    lv_obj_set_user_data(r, (void *)(uintptr_t)h->ip);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r, 16, 0);
    const char *glyph;
    lv_color_t col;
    kind_look(h, &glyph, &col);
    round_icon(r, glyph, col, 72, &aos_sym_44);
    int32_t tw = w - 16 * 2 - 28 - 72 - 16;
    lv_obj_t *txt = box(r, tw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 4, 0);
    char ip[16], t[160], ms[16];
    nt_ip_str(h->ip, ip, sizeof ip);
    lv_obj_t *name = aos_label(txt, h->name[0] ? h->name : ip, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(name, tw);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    fmt_ms(ms, sizeof ms, h->rtt_ms > 0 ? h->rtt_ms : -1);
    if (h->name[0]) snprintf(t, sizeof t, "%s · %s", ip, aos_tr(nt_guess(h)));
    else snprintf(t, sizeof t, "%s", aos_tr(nt_guess(h)));
    if (h->rtt_ms > 0) { size_t n = strlen(t); snprintf(t + n, sizeof t - n, " · %s ms", ms); }
    lv_obj_t *sub = aos_label(txt, t, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(sub, tw);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    if (h->nports) {
        lv_obj_t *pr = wrap_row(txt, tw, 8);
        lv_obj_set_style_pad_top(pr, 6, 0);
        for (int i = 0; i < h->nports; i++) port_tag(pr, h->ports[i], false);
    }
    lv_obj_t *chev = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
    lv_obj_add_flag(chev, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 32, 0);
    aos_make_decorative(txt);
}

static uint32_t hosts_sig(const nt_scan_t *s)
{
    uint32_t h = (uint32_t)s->n * 2654435761u;
    for (int i = 0; i < s->n; i++) {
        const nt_host_t *x = &s->hosts[i];
        h = h * 31 + x->ip + (uint32_t)x->nports * 7919u + x->mdns + (uint32_t)(uint8_t)x->name[0] + (x->icmp ? 3 : 0);
        for (int k = 0; k < x->nports; k++) h = h * 17 + x->ports[k];
    }
    return h;
}

static void hosts_list_build(void)
{
    page_t *g = &U.pg;
    if (!g->h_list) return;
    lv_obj_clean(g->h_list);
    int32_t w = g->h_list_w;
    nt_host_t *hs = nt_big_calloc(NT_MAX_HOSTS, sizeof *hs);
    if (!hs) return;
    nt_lock();
    const nt_scan_t *s = nt_scan();
    int n = s->n, state = s->state;
    memcpy(hs, s->hosts, sizeof(nt_host_t) * (size_t)n);
    nt_unlock();
    for (int i = 0; i < n; i++) host_row(g->h_list, &hs[i], w);
    if (!n) {
        lv_obj_t *e = box(g->h_list, w, 260);
        lv_obj_align(aos_label(e, AOS_SYM_RADAR, &aos_sym_72, lv_color_hex(0x48484A)), LV_ALIGN_TOP_MID, 0, 30);
        lv_obj_t *m = aos_label(e, state == NT_BUSY ? _("Buscando equipos…") : _("Todavía no se buscó"), aos_font_body, AOS_C_DIM);
        lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 140);
    }
    free(hs);
    g->h_built_ms = (uint32_t)aos_hal_uptime_ms();
}

static void detail_refresh(void);

static void hosts_refresh(void)
{
    page_t *g = &U.pg;
    if (!g->h_btn) return;
    nt_lock();
    const nt_scan_t *s = nt_scan();
    uint32_t seq = s->seq;
    bool changed = seq != g->h_seen;
    int state = s->state, phase = s->phase, done = s->done, total = s->total, n = s->n;
    bool icmp = s->icmp, full = s->full;
    uint32_t el = s->elapsed_ms;
    char range[40], err[96];
    snprintf(range, sizeof range, "%s", s->range);
    snprintf(err, sizeof err, "%s", s->err);
    uint32_t sig = hosts_sig(s);
    nt_unlock();
    if (changed) {
        g->h_seen = seq;
        bool busy = state == NT_BUSY;
        pill_set(g->h_btn, busy ? AOS_SYM_STOP : AOS_SYM_RADAR, busy ? _("Detener") : _("Buscar equipos"), busy ? AOS_C_RED : C_NET_D);
        char t[200], a[24];
        if (range[0]) lv_label_set_text_fmt(g->h_cap, n == 1 ? _("%s · %d encontrado") : _("%s · %d encontrados"), range, n);
        else lv_label_set_text(g->h_cap, _("Los equipos de tu red y lo que tienen abierto"));
        fmt_elapsed(a, sizeof a, el);
        if (state == NT_BUSY) {
            static const char *const PH[3] = { N_("Ping a todas las direcciones"), N_("Probando los puertos conocidos"), N_("Buscando los nombres (mDNS)") };
            if (phase < 2 && total) snprintf(t, sizeof t, "%s… %d/%d · %s", aos_tr(PH[phase]), done, total, a);
            else snprintf(t, sizeof t, "%s… %s", aos_tr(PH[phase > 2 ? 2 : phase]), a);
            /* the three phases share the bar: a third, a half, the rest */
            if (phase == 1 && total) bar_set(g->h_bar, 333 + done * 500 / total, 1000);
            else if (phase == 0 && total) bar_set(g->h_bar, done * 333 / total, 1000);
            else bar_set(g->h_bar, 900, 1000);
            lv_obj_set_style_text_color(g->h_state, AOS_C_DIM, 0);
        } else if (state == NT_FAILED) {
            snprintf(t, sizeof t, "%s", aos_tr(err));
            bar_set(g->h_bar, 0, 1);
            lv_obj_set_style_text_color(g->h_state, AOS_C_ORANGE, 0);
        } else if (state == NT_DONE) {
            snprintf(t, sizeof t, n == 1 ? _("Listo en %s: %d equipo%s") : _("Listo en %s: %d equipos%s"), a, n,
                     full ? _(", también los que no responden ping") : "");
            if (!icmp) { size_t k = strlen(t); snprintf(t + k, sizeof t - k, "  ·  %s", _("sin ICMP: sólo por TCP")); }
            bar_set(g->h_bar, 1, 1);
            lv_obj_set_style_text_color(g->h_state, AOS_C_DIM, 0);
        } else {
            snprintf(t, sizeof t, "%s", n ? _("Detenido: quedan los que se encontraron hasta ahí") : _("Rápido: los que responden ping. Completo: también los que no, tarda cerca de un minuto."));
            bar_set(g->h_bar, 0, 1);
            lv_obj_set_style_text_color(g->h_state, AOS_C_DIM, 0);
        }
        lv_label_set_text(g->h_state, t);
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (sig != g->h_sig && now - g->h_built_ms > 400) {
        g->h_sig = sig;
        hosts_list_build();
    }
}

static void detail_refresh(void)
{
    page_t *g = &U.pg;
    if (!g->d_btn || !S.sel) return;
    nt_ports_t r;
    nt_lock();
    const nt_ports_t *s = nt_ports();
    uint32_t seq = s->seq;
    if (seq == g->d_seen) { nt_unlock(); return; }
    r = *s;
    nt_unlock();
    g->d_seen = seq;
    bool mine = r.ip == S.sel;
    bool busy = r.state == NT_BUSY && mine;
    pill_set(g->d_btn, busy ? AOS_SYM_STOP : AOS_SYM_MAGNIFY, busy ? _("Detener") : _("Puertos 1-1024"), busy ? AOS_C_RED : C_NET_D);
    char t[160], a[24];
    if (!mine || (r.state == NT_IDLE && !r.done)) {
        lv_label_set_text(g->d_state, _("Prueba del 1 al 1024 y los altos de siempre (1883, 8123, 6053, 8080...)."));
        bar_set(g->d_bar, 0, 1);
    } else {
        fmt_elapsed(a, sizeof a, r.elapsed_ms);
        if (busy) snprintf(t, sizeof t, _("%d/%d · %d abiertos · %s"), r.done, r.total, r.nopen, a);
        else if (r.state == NT_FAILED) snprintf(t, sizeof t, "%s", aos_tr(r.err));
        else if (r.state == NT_IDLE) snprintf(t, sizeof t, _("Detenido en %d/%d · %d abiertos"), r.done, r.total, r.nopen);
        else snprintf(t, sizeof t, _("%d abiertos de %d, en %s%s"), r.nopen, r.total, a,
                      !r.nopen && !r.closed ? _(" · no contestó nada: puede tener un firewall") : "");
        lv_label_set_text(g->d_state, t);
        bar_set(g->d_bar, r.done, r.total);
    }
    if (mine && g->d_list && r.nopen != g->d_nopen) {
        g->d_nopen = r.nopen;
        lv_obj_clean(g->d_list);
        for (int i = 0; i < r.nopen; i++) port_tag(g->d_list, r.open[i], true);
    }
}

static void build_detail(lv_obj_t *parent, int32_t w)
{
    page_t *g = &U.pg;
    nt_host_t h;
    memset(&h, 0, sizeof h);
    h.ip = S.sel;
    bool known = find_host(S.sel, &h);
    char ip[16];
    nt_ip_str(S.sel, ip, sizeof ip);
    if (!U.land) {
        lv_obj_t *back = row(parent, w, 64, 6);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back, detail_close_cb, LV_EVENT_CLICKED, NULL);
        aos_label(back, AOS_SYM_CHEVRON_LEFT, &aos_sym_28, C_NET);
        aos_label(back, "Hosts", aos_font_body, C_NET);
        aos_make_decorative(lv_obj_get_child(back, 0));
        aos_make_decorative(lv_obj_get_child(back, 1));
    }
    lv_obj_t *top = vcard(parent, w, 24, 10);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const char *glyph;
    lv_color_t col;
    kind_look(&h, &glyph, &col);
    round_icon(top, glyph, col, 120, &aos_sym_72);
    lv_obj_t *nm = aos_label(top, h.name[0] ? h.name : ip, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(nm, w - 48);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    char t[160], ms[16];
    snprintf(t, sizeof t, "%s%s%s", h.name[0] ? ip : "", h.name[0] ? " · " : "", aos_tr(nt_guess(&h)));
    lv_obj_t *kd = aos_label(top, t, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(kd, w - 48);
    lv_obj_set_style_text_align(kd, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(kd, LV_LABEL_LONG_MODE_WRAP);
    if (known) {
        fmt_ms(ms, sizeof ms, h.rtt_ms > 0 ? h.rtt_ms : -1);
        if (h.icmp) snprintf(t, sizeof t, _("Respondió el ping en %s ms"), ms);
        else if (h.arp_only) snprintf(t, sizeof t, "%s", _("No responde el ping; contestó por ARP"));
        else snprintf(t, sizeof t, "%s", _("No responde el ping; contestó por TCP"));
        if (h.has_mac) {
            size_t n = strlen(t);
            snprintf(t + n, sizeof t - n, "\nMAC %02X:%02X:%02X:%02X:%02X:%02X",
                     h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
        }
        caption(top, t, w - 48);
        lv_obj_set_style_text_align(lv_obj_get_child(top, -1), LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *br = wrap_row(parent, w, 14);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    pill(br, AOS_SYM_PULSE, "Ping", AOS_C_GREEN, detail_ping_cb, NULL);
    g->d_btn = pill(br, AOS_SYM_MAGNIFY, _("Puertos 1-1024"), C_NET_D, detail_ports_cb, NULL);

    if (h.nports) {
        section(parent, _("PUERTOS CONOCIDOS"));
        lv_obj_t *kr = wrap_row(parent, w, 10);
        for (int i = 0; i < h.nports; i++) port_tag(kr, h.ports[i], true);
    }
    section(parent, _("TODOS LOS PUERTOS"));
    lv_obj_t *pc = vcard(parent, w, 20, 12);
    g->d_bar = progress(pc, w - 40);
    g->d_state = caption(pc, "", w - 40);
    g->d_list = wrap_row(pc, w - 40, 10);
    g->d_nopen = -1;

    /* what it announces */
    char ann[6][160];
    int na = 0;
    nt_lock();
    const nt_mdns_t *m = nt_mdns();
    for (int i = 0; i < m->n && na < 6; i++) {
        if (m->svc[i].ip != S.sel) continue;
        int ti = nt_mdns_type_index(m->svc[i].type);
        snprintf(ann[na++], sizeof ann[0], "%s  ·  %s :%u", m->svc[i].instance,
                 ti >= 0 ? aos_tr(NT_MDNS_TYPES[ti].label) : m->svc[i].type, m->svc[i].port);
    }
    nt_unlock();
    if (na) {
        section(parent, _("ANUNCIA POR mDNS"));
        for (int i = 0; i < na; i++) caption(parent, ann[i], w);
    }
    g->d_seen = 0xFFFFFFFFu;
    detail_refresh();
}

static void build_hosts(void)
{
    page_t *g = &U.pg;
    lv_obj_t *left, *right;
    int32_t lw, rw;
    split(U.content, 660, &left, &right, &lw, &rw);
    if (!U.land && S.sel) {
        build_detail(left, lw);
        return;
    }
    header(left, lw, "Hosts", &g->h_cap);
    lv_obj_t *c = vcard(left, lw, 20, 14);
    lv_obj_t *br = wrap_row(c, lw - 40, 12);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    g->h_btn = pill(br, AOS_SYM_RADAR, _("Buscar equipos"), C_NET_D, hosts_scan_cb, NULL);
    chip(br, _("Rápido"), !S.full, mode_cb, (void *)0);
    chip(br, _("Completo"), S.full, mode_cb, (void *)1);
    g->h_bar = progress(c, lw - 40);
    g->h_state = caption(c, "", lw - 40);
#ifdef AOS_SIM
    caption(left, _("Simulador: barre 127.0.0.1, o el rango que diga la variable P4_SIM_NET_SCAN."), lw);
#endif
    g->h_list = box(left, lw, LV_SIZE_CONTENT);
    g->h_list_w = lw;
    lv_obj_set_flex_flow(g->h_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g->h_list, 12, 0);
    if (U.land) {
        g->h_right = right;
        g->h_rw = rw;
        if (S.sel) build_detail(right, rw);
        else {
            lv_obj_t *e = box(right, rw, 420);
            lv_obj_align(aos_label(e, AOS_SYM_LAN, &aos_sym_72, lv_color_hex(0x48484A)), LV_ALIGN_TOP_MID, 0, 120);
            lv_obj_t *m = aos_label(e, _("Tocá un equipo para ver todos sus puertos"), aos_font_body, AOS_C_DIM);
            lv_obj_set_width(m, rw - 40);
            lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 230);
        }
    }
    g->h_seen = 0xFFFFFFFFu;
    g->h_sig = 0;
    g->h_built_ms = 0;
    hosts_list_build();
    nt_lock();
    g->h_sig = hosts_sig(nt_scan());
    nt_unlock();
    hosts_refresh();
}

/* -------------------------------------------------------------------------- */
/* WiFi                                                                        */
/* -------------------------------------------------------------------------- */

static const char *auth_name(uint8_t a)
{
    static const char *const N[] = { N_("Abierta"), "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "Enterprise", "?" };
    return a < sizeof N / sizeof N[0] ? aos_tr(N[a]) : "?";
}

static const char *rssi_glyph(int rssi)
{
    return rssi >= -55 ? AOS_SYM_WIFI_STRENGTH_4 : rssi >= -67 ? AOS_SYM_WIFI_STRENGTH_3
         : rssi >= -78 ? AOS_SYM_WIFI_STRENGTH_2 : AOS_SYM_WIFI_STRENGTH_1;
}

static lv_color_t rssi_color(int rssi)
{
    return rssi >= -55 ? AOS_C_GREEN : rssi >= -67 ? C_NET : rssi >= -75 ? AOS_C_YELLOW : rssi >= -85 ? AOS_C_ORANGE : AOS_C_RED;
}

static const char *rssi_word(int rssi)
{
    return rssi >= -55 ? _("Excelente") : rssi >= -67 ? _("Buena") : rssi >= -75 ? _("Regular") : rssi >= -85 ? _("Débil") : _("Muy débil");
}

static lv_color_t ssid_color(const char *ssid)
{
    static const uint32_t PAL[] = { 0x38BDF8, 0x30D158, 0xFF9F0A, 0xBF5AF2, 0xFF375F, 0xFFD60A, 0x40C8E0, 0xF472B6, 0xA3E635, 0xFB923C };
    uint32_t h = 2166136261u;
    for (const char *p = ssid; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return lv_color_hex(PAL[h % (sizeof PAL / sizeof PAL[0])]);
}

/* The 5 GHz channels, in order: the x axis of their graph. */
static const uint8_t CH5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                               132, 136, 140, 144, 149, 153, 157, 161, 165 };
#define N_CH5 (int)(sizeof CH5 / sizeof CH5[0])

/* Position of a 5 GHz channel number on that axis, in slots (fractional
 * between two listed ones). */
static float ch5_pos(float ch)
{
    for (int i = 0; i < N_CH5 - 1; i++)
        if (ch >= CH5[i] && ch <= CH5[i + 1] && CH5[i + 1] - CH5[i] == 4) return i + (ch - CH5[i]) / 4.0f;
    for (int i = 0; i < N_CH5; i++) if (ch <= CH5[i]) return (float)i;
    return (float)(N_CH5 - 1);
}

/* The centre of an AP's channel block and its half width, in the graph's
 * units (2,4 GHz: channel numbers; 5 GHz: slots). */
static void ap_span(const aos_wifi_ap_ex_t *a, bool five, float *centre, float *half)
{
    int w = a->width ? a->width : 20;
    if (!five) {
        /* 20 MHz is +-2 channels of 5 MHz; 40 MHz sits on primary and
         * secondary, 4 channels apart. */
        float c = a->channel;
        if (w >= 40) c += a->second >= 0 ? 2 : -2;
        *centre = c;
        *half = w >= 40 ? 4 : 2;
        return;
    }
    int blk = w / 20;                       /* slots the block covers */
    float c = a->channel;
    if (blk > 1) {
        int base = a->channel >= 149 ? 149 : a->channel >= 100 ? 100 : 36;
        int span = blk * 4;
        int start = base + ((a->channel - base) / span) * span;
        c = start + (span - 4) / 2.0f;
    }
    *centre = ch5_pos(c);
    *half = blk / 2.0f + 0.1f;
}

static bool same_bssid(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }

static void channels_paint(lv_layer_t *layer, const lv_area_t *area, bool five)
{
    lv_area_t c = *area;
    const lv_font_t *f = aos_font_tiny;
    int32_t lh = lv_font_get_line_height(f);
    int32_t x0 = c.x1 + 70, x1 = c.x2 - 24, y0 = c.y1 + 20 + lh, y1 = c.y2 - 16 - lh;
    float lo = five ? -0.8f : -1.0f, hi = five ? N_CH5 - 1 + 0.8f : 15.0f;
    char t[40];
    /* grid: dBm */
    for (int d = -90; d <= -30; d += 20) {
        int32_t y = y1 - (int32_t)((y1 - y0) * (d + 100) / 75.0f);
        draw_hline(layer, x0, x1, y, AOS_C_CARD2, 2, false);
        snprintf(t, sizeof t, "%d", d);
        draw_text(layer, t, f, AOS_C_DIM, c.x1 + 6, y - lh / 2, x0 - 10, LV_TEXT_ALIGN_RIGHT);
    }
    draw_text(layer, "dBm", f, AOS_C_DIM, c.x1 + 6, y0 - lh - 4, x0 + 40, LV_TEXT_ALIGN_LEFT);
    /* channel numbers */
    int nlab = five ? N_CH5 : 13;
    for (int i = 0; i < nlab; i++) {
        float v = five ? (float)i : (float)(i + 1);
        int32_t x = x0 + (int32_t)((x1 - x0) * (v - lo) / (hi - lo));
        if (five && U.land == false && i % 2) continue;         /* too tight upright */
        snprintf(t, sizeof t, "%d", five ? CH5[i] : i + 1);
        draw_text(layer, t, f, AOS_C_DIM, x - 30, y1 + 8, x + 30, LV_TEXT_ALIGN_CENTER);
    }
    int drawn = 0;
    lv_area_t labels[NT_MAX_APS];
    int nlabels = 0;
    for (int pass = 0; pass < 2; pass++) {          /* the connected one on top */
        for (int k = 0; k < U.w_n; k++) {
            const aos_wifi_ap_ex_t *a = &U.w_aps[k];
            if ((a->channel > 14) != five) continue;
            bool cur = U.w_cur_ok && same_bssid(a->bssid, U.w_cur.bssid);
            if (cur != (pass == 1)) continue;
            drawn++;
            float cen, half;
            ap_span(a, five, &cen, &half);
            lv_color_t col = cur ? lv_color_white() : ssid_color(a->ssid);
            float rs = a->rssi < -100 ? -100 : a->rssi > -25 ? -25 : a->rssi;
            float hpx = (y1 - y0) * (rs + 100) / 75.0f;
            float px_per = (x1 - x0) / (hi - lo);
            float xc = x0 + (cen - lo) * px_per, xh = half * px_per;
            int32_t prevx = 0, prevy = 0;
            int steps = (int)(2 * xh / 4);
            if (steps < 8) steps = 8;
            for (int k = 0; k <= steps; k++) {
                float u = -1 + 2.0f * k / steps;
                int32_t x = (int32_t)(xc + u * xh);
                int32_t y = y1 - (int32_t)(hpx * sqrtf(fmaxf(0, 1 - u * u)));
                if (x < x0 || x > x1) { prevx = 0; continue; }
                if (prevx && x > prevx) fill_rect(layer, prevx, (prevy + y) / 2, x - 1, y1, col, cur ? LV_OPA_40 : LV_OPA_20, 0);
                if (prevx) {
                    lv_draw_line_dsc_t d;
                    lv_draw_line_dsc_init(&d);
                    d.p1.x = prevx; d.p1.y = prevy;
                    d.p2.x = x; d.p2.y = y;
                    d.color = col;
                    d.width = cur ? 5 : 3;
                    d.round_start = d.round_end = 1;
                    lv_draw_line(layer, &d);
                }
                prevx = x;
                prevy = y;
            }
            const char *nm = a->ssid[0] ? a->ssid : _("(oculta)");
            lv_point_t sz;
            lv_text_get_size(&sz, nm, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_area_t la = { (int32_t)xc - sz.x / 2 - 4, y1 - (int32_t)hpx - lh - 2, (int32_t)xc + sz.x / 2 + 4, y1 - (int32_t)hpx - 2 };
            /* up a line while it would sit on another name */
            for (int tries = 0; tries < 4; tries++) {
                bool clash = false;
                for (int q = 0; q < nlabels && !clash; q++) {
                    lv_area_t *b = &labels[q];
                    clash = la.x1 <= b->x2 && la.x2 >= b->x1 && la.y1 <= b->y2 && la.y2 >= b->y1;
                }
                if (!clash) break;
                la.y1 -= lh;
                la.y2 -= lh;
            }
            if (la.y1 < c.y1 + 4) { la.y2 += c.y1 + 4 - la.y1; la.y1 = c.y1 + 4; }
            if (nlabels < (int)(sizeof labels / sizeof labels[0])) labels[nlabels++] = la;
            draw_text(layer, nm, f, col, la.x1 - 100, la.y1, la.x2 + 100, LV_TEXT_ALIGN_CENTER);
        }
    }
    if (!drawn) draw_text(layer, U.w_n ? _("Nada en esta banda") : _("Escaneando…"), aos_font_small, AOS_C_DIM, x0, (y0 + y1) / 2 - 14, x1, LV_TEXT_ALIGN_CENTER);
}

static void trace_paint(lv_layer_t *layer, const lv_area_t *area, bool five)
{
    (void)five;
    lv_area_t c = *area;
    const lv_font_t *f = aos_font_tiny;
    int32_t lh = lv_font_get_line_height(f);
    int32_t x0 = c.x1 + 70, x1 = c.x2 - 20, y0 = c.y1 + 16, y1 = c.y2 - 16 - lh;
    int mn = 0, mx = -120;
    for (int i = 0; i < U.w_rn; i++) {
        int v = U.w_rssi[i];
        if (!v) continue;
        if (!mn || v < mn) mn = v;
        if (v > mx) mx = v;
    }
    int top, bot;
    if (!mn) { top = -30; bot = -90; }
    else {
        top = ((mx + 5) / 5) * 5 + 5;
        bot = ((mn - 5) / 5) * 5 - 5;
        if (top - bot < 20) { int m = (top + bot) / 2; top = m + 10; bot = m - 10; }
        if (top > -10) top = -10;
    }
    char t[16];
    for (int k = 0; k <= 2; k++) {
        int v = bot + (top - bot) * k / 2;
        int32_t y = y1 - (y1 - y0) * k / 2;
        draw_hline(layer, x0, x1, y, AOS_C_CARD2, 2, false);
        snprintf(t, sizeof t, "%d", v);
        draw_text(layer, t, f, AOS_C_DIM, c.x1 + 6, y - lh / 2, x0 - 10, LV_TEXT_ALIGN_RIGHT);
    }
    draw_text(layer, _("hace 2 min"), f, AOS_C_DIM, x0, y1 + 6, x0 + 300, LV_TEXT_ALIGN_LEFT);
    draw_text(layer, _("ahora"), f, AOS_C_DIM, x1 - 200, y1 + 6, x1, LV_TEXT_ALIGN_RIGHT);
    float step = (float)(x1 - x0) / (NT_RSSI_HIST - 1);
    int off = NT_RSSI_HIST - U.w_rn;
    int32_t px = 0, py = 0;
    for (int i = 0; i < U.w_rn; i++) {
        int v = U.w_rssi[i];
        if (!v) { px = 0; continue; }
        int32_t x = x0 + (int32_t)((off + i) * step);
        int32_t y = y1 - (int32_t)((y1 - y0) * (float)(v - bot) / (float)(top - bot));
        if (px) {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1.x = px; d.p1.y = py;
            d.p2.x = x; d.p2.y = y;
            d.color = rssi_color(v);
            d.width = 4;
            d.round_start = d.round_end = 1;
            lv_draw_line(layer, &d);
        }
        px = x;
        py = y;
    }
}

static void graph_gone(lv_event_t *e)
{
    aos_hal_io_free(lv_event_get_user_data(e));
}

static void graph_init(graph_t *gr, lv_obj_t *holder, int32_t w, int32_t h, paint_fn paint, bool five, bool rounded)
{
    memset(gr, 0, sizeof *gr);
    gr->holder = holder;
    gr->w = w;
    gr->h = h;
    gr->paint = paint;
    gr->five = five;
    gr->cf = rounded ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB565;
    gr->bg = AOS_C_CARD;
}

/* Draws the graph into its picture; the picture is made the first time,
 * so one that stays hidden (5 GHz, which this board's C6 cannot hear)
 * costs nothing. */
static void graph_paint(graph_t *gr)
{
    if (!gr->holder || lv_obj_has_flag(gr->holder, LV_OBJ_FLAG_HIDDEN)) return;
    if (!gr->canvas) {
        size_t bytes = (size_t)gr->w * gr->h * (gr->cf == LV_COLOR_FORMAT_RGB565 ? 2 : 4);
        gr->px = aos_hal_io_alloc(bytes);
        if (!gr->px) return;
        gr->canvas = lv_canvas_create(gr->holder);
        lv_canvas_set_buffer(gr->canvas, gr->px, gr->w, gr->h, gr->cf);
        lv_obj_set_pos(gr->canvas, 0, 0);
        lv_obj_add_event_cb(gr->canvas, graph_gone, LV_EVENT_DELETE, gr->px);
    }
    if (gr->cf == LV_COLOR_FORMAT_RGB565) lv_canvas_fill_bg(gr->canvas, gr->bg, LV_OPA_COVER);
    else lv_canvas_fill_bg(gr->canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(gr->canvas, &layer);
    lv_area_t c = { 0, 0, gr->w - 1, gr->h - 1 };
    gr->paint(&layer, &c, gr->five);
    lv_canvas_finish_layer(gr->canvas, &layer);       /* drawn when it returns: stack texts are safe */
}

static void wifi_scan_cb(lv_event_t *e) { nt_wifi_scan(); U.pg.w_seen = 0xFFFFFFFFu; }

static void wifi_list_build(void)
{
    page_t *g = &U.pg;
    if (!g->w_list) return;
    lv_obj_clean(g->w_list);
    int32_t w = g->w_list_w;
    for (int k = 0; k < U.w_n; k++) {
        const aos_wifi_ap_ex_t *a = &U.w_aps[k];
        bool cur = U.w_cur_ok && same_bssid(a->bssid, U.w_cur.bssid);
        lv_obj_t *r = box(g->w_list, w, 92);
        lv_obj_set_style_pad_hor(r, 20, 0);
        if (k) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_t *gl = aos_label(r, rssi_glyph(a->rssi), &aos_sym_44, rssi_color(a->rssi));
        lv_obj_align(gl, LV_ALIGN_LEFT_MID, 0, 0);
        char t[120], mac[24];
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5]);
        lv_obj_t *nm = aos_label(r, a->ssid[0] ? a->ssid : _("(red oculta)"), aos_font_body, a->ssid[0] ? (cur ? C_NET : AOS_C_TEXT) : AOS_C_DIM);
        lv_obj_set_width(nm, w - 40 - 64 - 170);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 64, 12);
        snprintf(t, sizeof t, _("canal %u · %u MHz · %s · %s"), a->channel, a->width ? a->width : 20, auth_name(a->auth), mac);
        lv_obj_t *sub = aos_label(r, t, aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(sub, w - 40 - 64 - 120);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 64, -14);
        snprintf(t, sizeof t, "%d dBm", a->rssi);
        lv_obj_align(aos_label(r, t, aos_font_small, AOS_C_TEXT), LV_ALIGN_RIGHT_MID, a->auth ? -40 : 0, 0);
        if (a->auth) lv_obj_align(aos_label(r, AOS_SYM_LOCK, &aos_sym_28, AOS_C_DIM), LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (!U.w_n) {
        lv_obj_t *r = box(g->w_list, w, 92);
        lv_obj_center(aos_label(r, _("Escaneando…"), aos_font_body, AOS_C_DIM));
    }
}

static void wifi_refresh(void)
{
    page_t *g = &U.pg;
    if (!g->w_list) return;
    nt_lock();
    const nt_wifi_t *s = nt_wifi();
    uint32_t seq = s->seq, scanned = s->scanned_ms;
    int state = s->state;
    if (seq == g->w_seen) { nt_unlock(); return; }
    g->w_seen = seq;
    bool rescan = scanned != g->w_scanned;
    if (rescan) { U.w_n = s->n; memcpy(U.w_aps, s->aps, sizeof U.w_aps); }
    U.w_rn = s->rssi_n;
    memcpy(U.w_rssi, s->rssi, sizeof U.w_rssi);
    U.w_cur_ok = s->cur_ok;
    U.w_cur = s->cur;
    nt_unlock();

    char t[160];
    bool busy = state == NT_BUSY;
    pill_set(g->w_btn, busy ? AOS_SYM_TIMER_SAND : AOS_SYM_RESTART, busy ? _("Escaneando…") : _("Escanear"), busy ? AOS_C_CARD2 : C_NET_D);
    if (state == NT_FAILED) lv_label_set_text(g->w_when, _("El escaneo falló: ¿está prendida la radio?"));
    else if (scanned) {
        uint32_t ago = ((uint32_t)aos_hal_uptime_ms() - scanned) / 1000;
        int n24 = 0, n5 = 0;
        for (int k = 0; k < U.w_n; k++) { if (U.w_aps[k].channel > 14) n5++; else n24++; }
        snprintf(t, sizeof t, _("%d puntos de acceso (%d en 2,4 GHz, %d en 5 GHz) · hace %u s"), U.w_n, n24, n5, (unsigned)ago);
        lv_label_set_text(g->w_when, t);
    } else lv_label_set_text(g->w_when, _("Escaneando…"));

    if (U.w_cur_ok) {
        const aos_wifi_ap_ex_t *a = &U.w_cur;
        lv_label_set_text(g->w_ssid, a->ssid[0] ? a->ssid : _("(red oculta)"));
        lv_label_set_text_fmt(g->w_rssi, "%d", a->rssi);
        lv_obj_set_style_text_color(g->w_rssi, rssi_color(a->rssi), 0);
        lv_label_set_text(g->w_q, rssi_word(a->rssi));
        lv_obj_set_style_text_color(g->w_q, rssi_color(a->rssi), 0);
        lv_label_set_text(g->w_bars, rssi_glyph(a->rssi));
        lv_obj_set_style_text_color(g->w_bars, rssi_color(a->rssi), 0);
        snprintf(t, sizeof t, _("canal %u · %u MHz · %s · %02X:%02X:%02X:%02X:%02X:%02X"), a->channel, a->width ? a->width : 20,
                 auth_name(a->auth), a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5]);
        lv_label_set_text(g->w_info, t);
        lv_label_set_text_fmt(g->w_cap, _("Conectado a %s · %s"), a->ssid, aos_hal_net_ip());
    } else {
        lv_label_set_text(g->w_ssid, _("Sin conexión"));
        lv_label_set_text(g->w_rssi, "--");
        lv_obj_set_style_text_color(g->w_rssi, AOS_C_DIM, 0);
        lv_label_set_text(g->w_q, "");
        lv_label_set_text(g->w_bars, AOS_SYM_WIFI_STRENGTH_OFF_OUTLINE);
        lv_obj_set_style_text_color(g->w_bars, AOS_C_DIM, 0);
        lv_label_set_text(g->w_info, _("La placa no está asociada a ninguna red"));
        lv_label_set_text(g->w_cap, _("Las redes de alrededor y sus canales"));
    }
    graph_paint(&g->w_trace);
    if (rescan) {
        g->w_scanned = scanned;
        graph_paint(&g->w_g24);
        bool has5 = false;
        for (int k = 0; k < U.w_n; k++) if (U.w_aps[k].channel > 14) has5 = true;
        if (g->w_g5.holder) {
            lv_obj_set_flag(g->w_g5.holder, LV_OBJ_FLAG_HIDDEN, !has5);
            lv_obj_set_flag(g->w_l5, LV_OBJ_FLAG_HIDDEN, !has5);
            graph_paint(&g->w_g5);
        }
        wifi_list_build();
    }
}

static void build_wifi(void)
{
    page_t *g = &U.pg;
    lv_obj_t *left, *right;
    int32_t lw, rw;
    split(U.content, 560, &left, &right, &lw, &rw);
    header(left, lw, "WiFi", &g->w_cap);

    lv_obj_t *cc = vcard(left, lw, 22, 6);
    lv_obj_t *top = box(cc, lw - 44, 96);
    g->w_bars = aos_label(top, AOS_SYM_WIFI_STRENGTH_OUTLINE, &aos_sym_72, AOS_C_DIM);
    lv_obj_align(g->w_bars, LV_ALIGN_LEFT_MID, 0, 0);
    g->w_ssid = aos_label(top, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(g->w_ssid, lw - 44 - 90 - 170);
    lv_label_set_long_mode(g->w_ssid, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(g->w_ssid, LV_ALIGN_TOP_LEFT, 90, 6);
    g->w_q = aos_label(top, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(g->w_q, LV_ALIGN_BOTTOM_LEFT, 90, -6);
    lv_obj_t *rr = box(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(rr, 6, 0);
    lv_obj_align(rr, LV_ALIGN_RIGHT_MID, 0, 0);
    g->w_rssi = aos_label(rr, "--", aos_font_large, AOS_C_DIM);
    lv_obj_t *u = aos_label(rr, "dBm", aos_font_small, AOS_C_DIM);
    lv_obj_set_style_margin_bottom(u, 6, 0);
    g->w_info = aos_label(cc, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(g->w_info, lw - 44);
    lv_label_set_long_mode(g->w_info, LV_LABEL_LONG_MODE_WRAP);
    int32_t th = U.land ? 180 : 200;
    graph_init(&g->w_trace, box(cc, lw - 44, th), lw - 44, th, trace_paint, false, false);

    lv_obj_t *br = row(U.land ? left : right, U.land ? lw : rw, 80, 16);
    g->w_btn = pill(br, AOS_SYM_RESTART, _("Escanear"), C_NET_D, wifi_scan_cb, NULL);
    g->w_when = caption(U.land ? left : right, "", U.land ? lw : rw);

    section(right, _("CANALES 2,4 GHz"));
    int32_t h24 = U.land ? 300 : 330, h5 = U.land ? 260 : 280;
    graph_init(&g->w_g24, card(right, rw, h24), rw, h24, channels_paint, false, true);
    g->w_l5 = section(right, _("CANALES 5 GHz"));
    lv_obj_add_flag(g->w_l5, LV_OBJ_FLAG_HIDDEN);
    graph_init(&g->w_g5, card(right, rw, h5), rw, h5, channels_paint, true, true);
    lv_obj_add_flag(g->w_g5.holder, LV_OBJ_FLAG_HIDDEN);

    section(U.land ? left : right, _("REDES"));
    g->w_list = card(U.land ? left : right, U.land ? lw : rw, LV_SIZE_CONTENT);
    g->w_list_w = U.land ? lw : rw;
    lv_obj_set_flex_flow(g->w_list, LV_FLEX_FLOW_COLUMN);
#ifdef AOS_SIM
    caption(U.land ? left : right, _("Simulador: las redes son de ejemplo; en la placa las escanea el ESP32-C6, que sólo tiene 2,4 GHz."), U.land ? lw : rw);
#endif

    /* a fresh scan if the last one is old */
    nt_lock();
    uint32_t scanned = nt_wifi()->scanned_ms;
    int state = nt_wifi()->state;
    nt_unlock();
    if (state != NT_BUSY && (!scanned || (uint32_t)aos_hal_uptime_ms() - scanned > 30000)) nt_wifi_scan();
    g->w_seen = 0xFFFFFFFFu;
    g->w_scanned = 0xFFFFFFFFu;
    wifi_refresh();
}

/* -------------------------------------------------------------------------- */
/* mDNS                                                                        */
/* -------------------------------------------------------------------------- */

static void mdns_go_cb(lv_event_t *e) { nt_mdns_start(); U.pg.m_seen = 0xFFFFFFFFu; }

static void mfilter_cb(lv_event_t *e)
{
    S.mfilter = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void mdns_list_build(const nt_mdns_t *m)
{
    page_t *g = &U.pg;
    lv_obj_clean(g->m_list);
    lv_obj_clean(g->m_filters);
    int32_t w = g->m_list_w;
    int count[32] = { 0 };
    for (int i = 0; i < m->n; i++) {
        int ti = nt_mdns_type_index(m->svc[i].type);
        if (ti >= 0) count[ti]++;
    }
    char t[200];
    snprintf(t, sizeof t, _("Todos (%d)"), m->n);
    chip(g->m_filters, t, S.mfilter < 0, mfilter_cb, (void *)(intptr_t)-1);
    for (int ti = 0; ti < NT_MDNS_NTYPES; ti++) {
        if (!count[ti]) continue;
        snprintf(t, sizeof t, "%s (%d)", aos_tr(NT_MDNS_TYPES[ti].label), count[ti]);
        chip(g->m_filters, t, S.mfilter == ti, mfilter_cb, (void *)(intptr_t)ti);
    }
    int last = -2;
    lv_obj_t *grp = NULL;
    int shown = 0;
    for (int i = 0; i < m->n; i++) {
        const aos_mdns_svc_t *s = &m->svc[i];
        int ti = nt_mdns_type_index(s->type);
        if (S.mfilter >= 0 && ti != S.mfilter) continue;
        if (ti != last) {
            last = ti;
            snprintf(t, sizeof t, "%s  ·  %s", ti >= 0 ? aos_tr(NT_MDNS_TYPES[ti].label) : s->type, s->type);
            lv_obj_t *sec = section(g->m_list, t);
            lv_obj_set_style_pad_top(sec, shown ? 12 : 0, 0);
            grp = card(g->m_list, w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_COLUMN);
        }
        lv_obj_t *r = box(grp, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(r, 22, 0);
        lv_obj_set_style_pad_ver(r, 16, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 4, 0);
        if (lv_obj_get_child_count(grp) > 1) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_t *nm = aos_label(r, s->instance, aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(nm, w - 44);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        char ip[16];
        if (s->ip) nt_ip_str(s->ip, ip, sizeof ip);
        else snprintf(ip, sizeof ip, "?");
        snprintf(t, sizeof t, "%s%s  ·  %s:%u", s->host[0] ? s->host : "?", s->host[0] ? ".local" : "", ip, s->port);
        lv_obj_t *sub = aos_label(r, t, aos_font_caption, C_NET);
        lv_obj_set_width(sub, w - 44);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
        if (s->txt[0]) {
            lv_obj_t *tx = aos_label(r, s->txt, aos_font_tiny, AOS_C_DIM);
            lv_obj_set_width(tx, w - 44);
            lv_label_set_long_mode(tx, LV_LABEL_LONG_MODE_DOTS);
        }
        shown++;
    }
    if (!shown) {
        lv_obj_t *e = box(g->m_list, w, 260);
        lv_obj_align(aos_label(e, AOS_SYM_DNS, &aos_sym_72, lv_color_hex(0x48484A)), LV_ALIGN_TOP_MID, 0, 30);
        lv_obj_t *l = aos_label(e, m->state == NT_BUSY ? _("Preguntando en la red…") : _("Nadie anunció nada"), aos_font_body, AOS_C_DIM);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 140);
    }
}

static void mdns_refresh(void)
{
    page_t *g = &U.pg;
    if (!g->m_list) return;
    nt_mdns_t *m = nt_big_calloc(1, sizeof *m);
    if (!m) return;
    nt_lock();
    const nt_mdns_t *s = nt_mdns();
    if (s->seq == g->m_seen) { nt_unlock(); free(m); return; }
    *m = *s;
    nt_unlock();
    g->m_seen = m->seq;
    bool busy = m->state == NT_BUSY;
    pill_set(g->m_btn, busy ? AOS_SYM_TIMER_SAND : AOS_SYM_MAGNIFY, busy ? _("Preguntando…") : _("Buscar servicios"), busy ? AOS_C_CARD2 : C_NET_D);
    char a[24];
    fmt_elapsed(a, sizeof a, m->elapsed_ms);
    if (m->state == NT_FAILED) {
        lv_label_set_text(g->m_state, aos_tr(m->err));
        lv_obj_set_style_text_color(g->m_state, AOS_C_ORANGE, 0);
    } else {
        if (m->state == NT_DONE) lv_label_set_text_fmt(g->m_state, _("%d servicios, respondieron en %s"), m->n, a);
        else if (busy) lv_label_set_text(g->m_state, _("Preguntando por 15 tipos de servicio a la vez…"));
        else lv_label_set_text(g->m_state, "");
        lv_obj_set_style_text_color(g->m_state, AOS_C_DIM, 0);
    }
    if (!busy || !m->n) mdns_list_build(m);
    free(m);
}

static void build_mdns(void)
{
    page_t *g = &U.pg;
    lv_obj_t *left, *right;
    int32_t lw, rw;
    split(U.content, 440, &left, &right, &lw, &rw);
    lv_obj_t *cap;
    header(left, lw, "mDNS", &cap);
    lv_label_set_text(cap, _("Quién anuncia qué en la red (.local)"));
    lv_obj_t *c = vcard(left, lw, 20, 12);
    lv_obj_t *br = row(c, lw - 40, 80, 12);
    g->m_btn = pill(br, AOS_SYM_MAGNIFY, _("Buscar servicios"), C_NET_D, mdns_go_cb, NULL);
    g->m_state = caption(c, "", lw - 40);
    section(left, _("TIPOS"));
    g->m_filters = wrap_row(left, lw, 10);
#ifdef AOS_SIM
    caption(left, _("Simulador: son servicios de ejemplo; en la placa se le pregunta a la red de verdad."), lw);
#endif
    g->m_list = box(right, rw, LV_SIZE_CONTENT);
    g->m_list_w = rw;
    lv_obj_set_flex_flow(g->m_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g->m_list, 10, 0);
    nt_lock();
    int state = nt_mdns()->state;
    nt_unlock();
    if (state == NT_IDLE) nt_mdns_start();
    g->m_seen = 0xFFFFFFFFu;
    mdns_refresh();
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    kp_close();
    lv_obj_clean(U.content);
    memset(&U.pg, 0, sizeof U.pg);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_NET : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    if (S.tab == TAB_PING) build_ping();
    else if (S.tab == TAB_HOSTS) build_hosts();
    else if (S.tab == TAB_WIFI) build_wifi();
    else build_mdns();
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == S.tab && t == TAB_HOSTS && S.sel && !U.land) S.sel = 0;   /* a second tap goes back to the list */
    S.tab = t;
    settings_save();
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    nt_keepalive();
    nt_wifi_run();              /* the signal trace keeps filling on every tab */
    if (U.overlay) return;
    if (S.tab == TAB_PING) ping_refresh();
    else if (S.tab == TAB_HOSTS) { hosts_refresh(); detail_refresh(); }
    else if (S.tab == TAB_WIFI) wifi_refresh();
    else mdns_refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    if (!nt_init()) {
        lv_obj_center(aos_label(root, _("No hay memoria para abrir Red"), aos_font_body, AOS_C_DIM));
        return NULL;
    }
    nt_keepalive();
    settings_load();
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    const int32_t tab_h = U.land ? 96 : 116;
    U.content = box(root, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 8, 0);
    lv_obj_update_layout(U.content);
    U.cw = U.W - 2 * AOS_UI_PAD;
    U.ch = lv_obj_get_content_height(U.content);
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
        lv_obj_align(aos_label(t, TAB_NAME[i], aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    nt_wifi_run();
    build_page();
    U.timer = lv_timer_create(timer_cb, 125, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    U.overlay = U.ta = U.kb = NULL;
    memset(&U.pg, 0, sizeof U.pg);
    /* the service's threads notice the silence and stop in ~3 s */
}

static bool back(aos_app_t *self, void *inst)
{
    if (!U.content) return false;
    if (U.overlay) { kp_close(); return true; }
    if (S.tab == TAB_HOSTS && S.sel) { S.sel = 0; build_page(); return true; }
    return false;
}

void aos_app_net_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.net", .name = "Red", .icon = AOS_SYM_SERVER_NETWORK,
            .color_a = 0x38BDF8, .color_b = 0x0369A1,
            .order = 170,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
