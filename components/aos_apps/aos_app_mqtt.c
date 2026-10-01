/*
 * P4OS - MQTT: an explorer for a broker, over the aos_mqtt.c service.
 *
 * Three tabs:
 *
 *   Explorar  every topic the service has seen, as a tree folded by level
 *             (zigbee2mqtt > living_temp), with live counts; a row flashes
 *             when a message lands in it or, folded, under it. A search
 *             turns the tree into a flat list of the topics that match.
 *             Tapping a topic opens its sheet (portrait) or fills the panel
 *             on the right (landscape): the payload pretty-printed when it
 *             is JSON, whether it came retained, when, how many, and a chart
 *             of every number in it, one field at a time.
 *   Publicar  topic, payload, QoS and retain, and the recent publishes as
 *             one-tap buttons (kept in the preferences; a pin keeps one).
 *   Conexión  the broker, the user, the password, the client id and the
 *             subscription, typed on the screen's keyboard or read from
 *             mqtt.txt on the card; the state and the counters.
 *
 * The service keeps running when the app closes (the Macro pad publishes
 * through it); the screen only reads its table and queues publishes.
 */
#include "aos_apps.h"
#include "aos_mqtt.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"
#include "aos_mono.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_MQ       lv_color_hex(0xA78BFA)
#define C_MQ_D     lv_color_hex(0x6D28D9)
#define C_SHEET    lv_color_hex(0x161618)
#define C_JSON     lv_color_hex(0x40C8E0)

enum { TAB_EXP, TAB_PUB, TAB_CON, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { N_("Explorar"), N_("Publicar"), N_("Conexión") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_POUND, AOS_SYM_SEND, AOS_SYM_ACCESS_POINT };

#define MAX_ROWS    160             /* rows drawn in the tree at once */
#define MAX_EXP     128             /* unfolded nodes remembered */
#define MAX_DEPTH   12
#define N_FAV       10
#define FAV_TOPIC   100
#define FAV_PAYLOAD 120
#define LEAF_H      100
#define NODE_H      84
#define FLASH_MS    700

typedef struct {
    char    topic[FAV_TOPIC];
    char    payload[FAV_PAYLOAD];
    uint8_t qos;
    bool    retain, pinned;
} fav_t;

/* Kept across rotations and tab changes (PSRAM: the LVGL task only, cold). */
typedef struct {
    bool     loaded;
    int      tab;
    char     filter[64];
    uint32_t exp[MAX_EXP];
    int      nexp;
    char     sel[AOS_MQTT_TOPIC_MAX];
    char     sel_field[AOS_MQTT_FIELD_MAX];
    bool     sheet;
    char     pub_topic[AOS_MQTT_TOPIC_MAX];
    char     pub_payload[AOS_MQTT_PUB_MAX + 1];
    int      pub_qos;
    bool     pub_retain;
    fav_t    fav[N_FAV];
    int      nfav;
    uint32_t rate_rx, rate_ms;
    float    rate;
} state_t;

/* One row of the tree, as the model sees it. */
typedef struct {
    uint32_t key, seq, msgs;
    int16_t  slot;                  /* the topic's, -1 for a node */
    uint8_t  depth, kind;
    bool     node, open, retained, sel;
    uint16_t ntop;
    char     name[AOS_MQTT_TOPIC_MAX];
    char     sum[96];
    uint32_t count;
} vrow_t;

/* And as the screen draws it. */
typedef struct {
    lv_obj_t *o, *name, *sub, *count, *pin, *dot;
    uint32_t  key, seq, flash_ms;
    bool      node, selected;
} urow_t;

typedef struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay, *sheet, *ta, *kb;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    uint32_t seen, t_exp, t_det, t_con;
    /* Explorar */
    lv_obj_t *list, *st_dot, *st_text, *st_pill, *hdr_sub, *search_l, *search_x, *fold_g, *more;
    urow_t rows[MAX_ROWS];
    int nrows;
    uint32_t sig;
    /* the detail */
    lv_obj_t *dpanel, *dcol, *d_facts, *d_fields, *d_value, *d_chart, *d_hi, *d_lo, *d_span, *d_payload, *d_numwrap, *d_empty;
    lv_chart_series_t *d_ser;
    uint32_t d_seq;
    int d_nf;
    /* Publicar */
    lv_obj_t *p_topic, *p_payload, *p_favs, *p_sub;
    /* Conexión */
    lv_obj_t *c_dot, *c_state, *c_detail, *c_sw, *c_val[9];
} ui_t;

/* Everything big, in one block of PSRAM taken the first time the app opens
 * and kept (the state survives closing the app): AOS_BSS_PSRAM is not
 * honoured by this firmware, and malloc keeps blocks under 16 KB inside. */
typedef struct {
    state_t          S;
    ui_t             U;
    vrow_t           vr[MAX_ROWS];
    int16_t          order[AOS_MQTT_MAX_TOPICS];
    char             pretty[2048];
    char             text[AOS_MQTT_PUB_MAX + 1];
    char             pick[10][AOS_MQTT_TOPIC_MAX];
    aos_mqtt_topic_t dt;
    float            dv[AOS_MQTT_HIST];
    uint32_t         dtm[AOS_MQTT_HIST];
} mem_t;

static mem_t *A;
#define S        (A->S)
#define U        (A->U)
#define s_vr     (A->vr)
#define s_order  (A->order)
#define s_pretty (A->pretty)
#define s_text   (A->text)

#if defined(AOS_SIM)
static void *big_calloc(size_t n) { return calloc(1, n); }
#else
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#endif
static int s_nvr, s_more;

static void build_page(void);
static void exp_refresh(bool force);
static void detail_refresh(bool force);
static void sheet_close(void);

/* -------------------------------------------------------------------------- */
/* Little things                                                               */
/* -------------------------------------------------------------------------- */

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static void scpy(char *d, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l);
    d[l] = 0;
}

/* 12.34 -> "12,34", with only the decimals it needs (3 at most) */
static void fmt_num(char *out, size_t n, double v)
{
    if (!isfinite(v)) { scpy(out, n, "--"); return; }
    if (fabs(v) >= 1e6) snprintf(out, n, "%.0f", v);
    else {
        snprintf(out, n, "%.3f", v);
        char *d = strchr(out, '.');
        if (d) {
            char *e = out + strlen(out) - 1;
            while (e > d && *e == '0') *e-- = 0;
            if (e == d) *e = 0;
        }
    }
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
    if (!strcmp(out, "-0")) scpy(out, n, "0");
}

/* 12345 -> "12.345" */
static void fmt_int(char *out, size_t n, uint32_t v)
{
    char t[16];
    snprintf(t, sizeof t, "%u", (unsigned)v);
    size_t l = strlen(t), k = 0;
    for (size_t i = 0; i < l && k + 1 < n; i++) {
        out[k++] = t[i];
        size_t left = l - i - 1;
        if (left && left % 3 == 0 && k + 1 < n) out[k++] = '.';
    }
    out[k] = 0;
}

static void fmt_bytes(char *out, size_t n, uint32_t b)
{
    char t[12];
    if (b < 1024) { snprintf(out, n, "%u B", (unsigned)b); return; }
    if (b < 1048576) fmt_num(t, sizeof t, round(b / 102.4) / 10);
    else fmt_num(t, sizeof t, round(b / 104857.6) / 10);
    snprintf(out, n, "%s %s", t, b < 1048576 ? "KB" : "MB");
}

static void fmt_age(char *out, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s < 2) scpy(out, n, _("recién"));
    else if (s < 60) snprintf(out, n, _("hace %u s"), (unsigned)s);
    else if (s < 3600) snprintf(out, n, _("hace %u min"), (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, _("hace %u h"), (unsigned)(s / 3600));
    else snprintf(out, n, _("hace %u días"), (unsigned)(s / 86400));
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

/* One line, cut with an ellipsis: a DOTS label with an automatic height
 * wraps first and dots the last line, so the height is pinned to one line. */
static void dots(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}

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

static lv_obj_t *column(lv_obj_t *parent, int32_t w, int32_t h, bool scroll)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    if (scroll) {
        lv_obj_set_style_pad_bottom(c, 24, 0);
        lv_obj_set_scroll_dir(c, LV_DIR_VER);
    } else {
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    }
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
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
        lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    }
    lv_obj_center(aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    return c;
}

/* A tag that says something and does nothing: "Retenido", "QoS 1". */
static lv_obj_t *tag(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_set_style_pad_hor(c, 18, 0);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_center(aos_label(c, text, aos_font_caption, fg));
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

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, 76, 76);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT));
    return b;
}

static void state_look(aos_mqtt_state_t st, const char **text, lv_color_t *c)
{
    switch (st) {
    case AOS_MQTT_CONNECTED:    *text = _("Conectado");      *c = AOS_C_GREEN; break;
    case AOS_MQTT_CONNECTING:   *text = _("Conectando…");    *c = AOS_C_ORANGE; break;
    case AOS_MQTT_WAITING_NET:  *text = _("Sin red");        *c = AOS_C_ORANGE; break;
    case AOS_MQTT_REFUSED:      *text = _("Rechazado");      *c = AOS_C_RED; break;
    case AOS_MQTT_ERROR:        *text = _("Sin conexión");   *c = AOS_C_RED; break;
    case AOS_MQTT_UNCONFIGURED: *text = _("Sin configurar"); *c = AOS_C_DIM; break;
    default:                    *text = _("Apagado");        *c = AOS_C_DIM; break;
    }
}

/* -------------------------------------------------------------------------- */
/* Settings of the screen and the recent publishes                             */
/* -------------------------------------------------------------------------- */

static void fav_save(void)
{
    for (int i = 0; i < N_FAV; i++) {
        char key[16];
        snprintf(key, sizeof key, "mqtt_fav%d", i);
        if (i >= S.nfav) { aos_hal_pref_erase(key); continue; }
        const fav_t *f = &S.fav[i];
        char v[FAV_TOPIC + FAV_PAYLOAD + 8];
        int n = snprintf(v, sizeof v, "%c%c%c\x1f%s\x1f", f->pinned ? 'P' : '-', f->qos ? '1' : '0', f->retain ? 'R' : '-', f->topic);
        /* the simulator's file is one line per key: no newlines in it */
        for (const char *p = f->payload; *p && n + 1 < (int)sizeof v; p++) v[n++] = *p == '\n' ? '\x1e' : *p;
        v[n] = 0;
        aos_hal_pref_set_str(key, v);
    }
}

static void fav_load(void)
{
    S.nfav = 0;
    for (int i = 0; i < N_FAV; i++) {
        char key[16], v[FAV_TOPIC + FAV_PAYLOAD + 8];
        snprintf(key, sizeof key, "mqtt_fav%d", i);
        if (!aos_hal_pref_get_str(key, v, sizeof v) || strlen(v) < 5 || v[3] != '\x1f') continue;
        char *t = v + 4, *sep = strchr(t, '\x1f');
        if (!sep) continue;
        *sep = 0;
        fav_t *f = &S.fav[S.nfav++];
        f->pinned = v[0] == 'P';
        f->qos = v[1] == '1';
        f->retain = v[2] == 'R';
        scpy(f->topic, sizeof f->topic, t);
        scpy(f->payload, sizeof f->payload, sep + 1);
        for (char *p = f->payload; *p; p++) if (*p == '\x1e') *p = '\n';
    }
}

static void fav_add(const char *topic, const char *payload, int qos, bool retain)
{
    if (strlen(topic) >= FAV_TOPIC || strlen(payload) >= FAV_PAYLOAD) return;     /* too long to keep */
    bool pinned = false;
    for (int i = 0; i < S.nfav; i++) {
        fav_t *f = &S.fav[i];
        if (!strcmp(f->topic, topic) && !strcmp(f->payload, payload) && f->qos == qos && f->retain == retain) {
            pinned = f->pinned;
            memmove(S.fav + i, S.fav + i + 1, (size_t)(S.nfav - i - 1) * sizeof S.fav[0]);
            S.nfav--;
            break;
        }
    }
    int at = 0;
    if (!pinned) while (at < S.nfav && S.fav[at].pinned) at++;
    if (S.nfav == N_FAV) {
        int drop = -1;
        for (int i = S.nfav - 1; i >= 0; i--) if (!S.fav[i].pinned) { drop = i; break; }
        if (drop < 0) return;
        memmove(S.fav + drop, S.fav + drop + 1, (size_t)(S.nfav - drop - 1) * sizeof S.fav[0]);
        S.nfav--;
        if (at > S.nfav) at = S.nfav;
    }
    memmove(S.fav + at + 1, S.fav + at, (size_t)(S.nfav - at) * sizeof S.fav[0]);
    fav_t *f = &S.fav[at];
    scpy(f->topic, sizeof f->topic, topic);
    scpy(f->payload, sizeof f->payload, payload);
    f->qos = (uint8_t)qos;
    f->retain = retain;
    f->pinned = pinned;
    S.nfav++;
    fav_save();
}

static void settings_load(void)
{
    if (S.loaded) return;
    memset(&S, 0, sizeof S);
    S.loaded = true;
    int32_t v;
    if (aos_hal_pref_get_i32("mqtt_tab", &v) && v >= 0 && v < TAB_COUNT) S.tab = v;
    if (!aos_hal_pref_get_str("mqtt_ptopic", S.pub_topic, sizeof S.pub_topic)) S.pub_topic[0] = 0;
    if (!aos_hal_pref_get_str("mqtt_ppay", S.pub_payload, sizeof S.pub_payload)) S.pub_payload[0] = 0;
    S.pub_qos = aos_hal_pref_get_i32("mqtt_pqos", &v) ? v != 0 : 0;
    S.pub_retain = aos_hal_pref_get_i32("mqtt_pret", &v) ? v != 0 : false;
    fav_load();
}

static void pub_save(void)
{
    aos_hal_pref_set_str("mqtt_ptopic", S.pub_topic);
    char p[200];
    scpy(p, sizeof p, S.pub_payload);           /* the form keeps what fits on a line of the file */
    for (char *c = p; *c; c++) if (*c == '\n') *c = ' ';
    aos_hal_pref_set_str("mqtt_ppay", p);
    aos_hal_pref_set_i32("mqtt_pqos", S.pub_qos);
    aos_hal_pref_set_i32("mqtt_pret", S.pub_retain);
}

/* -------------------------------------------------------------------------- */
/* The text field (host names, topics, payloads)                               */
/* -------------------------------------------------------------------------- */

static void (*s_text_done)(const char *);
static lv_obj_t *s_sug;                     /* the topics offered under a topic field */

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = U.ta = U.kb = s_sug = NULL;
}

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    scpy(s_text, sizeof s_text, lv_textarea_get_text(U.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    overlay_close();
    if (done) done(s_text);
}

static void clear_text_cb(lv_event_t *e) { if (U.ta) lv_textarea_set_text(U.ta, ""); }

static void sug_pick_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_obj_get_child(lv_event_get_current_target(e), 0);
    if (U.ta && l) lv_textarea_set_text(U.ta, lv_label_get_text(l));
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Where a device of the table takes orders, from what it reports:
 * zigbee2mqtt/<dev> -> .../set, tasmota tele|stat/<dev>/... -> cmnd/<dev>/POWER,
 * ESPHome <node>/switch|light|fan/<x>/state -> .../command. "" if none. */
static void command_topic(const char *t, char *out, size_t n)
{
    out[0] = 0;
    const char *a = strchr(t, '/');
    if (!strncmp(t, "zigbee2mqtt/", 12) && a && !strchr(a + 1, '/') && strcmp(a + 1, "bridge")) {
        snprintf(out, n, "%.100s/set", t);
    } else if ((!strncmp(t, "tele/", 5) || !strncmp(t, "stat/", 5)) && a && strchr(a + 1, '/')) {
        const char *b = strchr(a + 1, '/');
        snprintf(out, n, "cmnd/%.*s/POWER", (int)(b - a - 1 < 80 ? b - a - 1 : 80), a + 1);
    } else {
        size_t l = strlen(t);
        if (l > 6 && !strcmp(t + l - 6, "/state") && (strstr(t, "/switch/") || strstr(t, "/light/") || strstr(t, "/fan/")))
            snprintf(out, n, "%.*s/command", (int)(l - 6 < 100 ? l - 6 : 100), t);
    }
}

/* Up to ten topics with what was typed in them: first where the devices
 * of the table take orders (the ones seen and the ones guessed), then the
 * rest, leaving out Home Assistant's discovery configs. */
static void sug_refresh(void)
{
    if (!s_sug || !U.ta) return;
    lv_obj_clean(s_sug);
    const char *typed = lv_textarea_get_text(U.ta);
    char (*pick)[AOS_MQTT_TOPIC_MAX] = A->pick;
    char *ptr[10];
    int n = 0, ncmd = 0;
    bool want_cfg = ci_contains(typed, "config") || ci_contains(typed, "homeassistant");
    aos_mqtt_lock();
    for (int pass = 0; pass < 2 && n < 10; pass++) {
        for (int i = 0; i < AOS_MQTT_MAX_TOPICS && n < 10; i++) {
            const aos_mqtt_topic_t *t = aos_mqtt_topic_at(i);
            if (!t) continue;
            char cand[AOS_MQTT_TOPIC_MAX];
            const char *last = strrchr(t->topic, '/');
            bool cmd = !strncmp(t->topic, "cmnd/", 5) || (last && (!strcmp(last, "/set") || !strcmp(last, "/command")));
            if (pass == 0) {
                if (cmd) scpy(cand, sizeof cand, t->topic);
                else command_topic(t->topic, cand, sizeof cand);
            } else {
                if (cmd || (!want_cfg && last && !strcmp(last, "/config"))) continue;
                scpy(cand, sizeof cand, t->topic);
            }
            if (!cand[0] || !ci_contains(cand, typed) || !strcmp(cand, typed)) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = !strcmp(pick[k], cand);
            if (dup) continue;
            scpy(pick[n], sizeof pick[n], cand);
            ptr[n] = pick[n];
            n++;
        }
        if (pass == 0) ncmd = n;
    }
    aos_mqtt_unlock();
    qsort(ptr, (size_t)ncmd, sizeof ptr[0], cmp_str);
    qsort(ptr + ncmd, (size_t)(n - ncmd), sizeof ptr[0], cmp_str);
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = chip(s_sug, ptr[i], false, sug_pick_cb, NULL);
        lv_obj_t *l = lv_obj_get_child(c, 0);
        lv_obj_set_style_text_font(l, &aos_mono_18, 0);
        if (i < ncmd) lv_obj_set_style_text_color(l, C_MQ, 0);
        lv_obj_set_style_max_width(c, U.W - 2 * AOS_UI_PAD, 0);
    }
}

static void ta_changed_cb(lv_event_t *e) { sug_refresh(); }

static void text_entry_ex(const char *title, const char *hint, const char *value, int max, bool password, bool suggest,
                          void (*done)(const char *))
{
    overlay_close();
    s_text_done = done;
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, U.land ? 20 : 30);
    int32_t ty = U.land ? 76 : 100;
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_max_length(U.ta, (uint32_t)max);
    lv_textarea_set_text(U.ta, value);
    if (password) lv_textarea_set_password_mode(U.ta, true);
    if (hint) lv_textarea_set_placeholder_text(U.ta, hint);
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD - 88, 88);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, ty);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    lv_obj_t *x = round_btn(U.overlay, AOS_SYM_BACKSPACE_OUTLINE, clear_text_cb, NULL);
    lv_obj_set_size(x, 76, 76);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, ty + 6);
    U.kb = lv_keyboard_create(U.overlay);
    int32_t kh = U.land ? U.H / 2 : U.H * 2 / 5;
    lv_obj_set_size(U.kb, U.W, kh);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_cb, LV_EVENT_ALL, NULL);
    if (suggest) {
        int32_t sy = ty + 88 + 20, sh = U.H - kh - sy - 12;
        if (sh >= 60) {
            s_sug = box(U.overlay, U.W - 2 * AOS_UI_PAD, sh);
            lv_obj_set_pos(s_sug, AOS_UI_PAD, sy);
            lv_obj_set_flex_flow(s_sug, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_gap(s_sug, 10, 0);
            lv_obj_add_flag(s_sug, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(s_sug, LV_DIR_VER);
            lv_obj_add_event_cb(U.ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
            sug_refresh();
        }
    }
}

static void text_entry(const char *title, const char *hint, const char *value, int max, bool password, void (*done)(const char *))
{
    text_entry_ex(title, hint, value, max, password, false, done);
}

/* -------------------------------------------------------------------------- */
/* The tree, as a list of rows                                                 */
/* -------------------------------------------------------------------------- */

static bool expanded(uint32_t key)
{
    for (int i = 0; i < S.nexp; i++) if (S.exp[i] == key) return true;
    return false;
}

static void set_expanded(uint32_t key, bool on)
{
    for (int i = 0; i < S.nexp; i++) {
        if (S.exp[i] != key) continue;
        if (!on) S.exp[i] = S.exp[--S.nexp];
        return;
    }
    if (on && S.nexp < MAX_EXP) S.exp[S.nexp++] = key;
}

/* '/' sorts before anything, so a level's children follow it at once */
static int cmp_topic(const void *a, const void *b)
{
    const char *x = aos_mqtt_topic_at(*(const int16_t *)a)->topic, *y = aos_mqtt_topic_at(*(const int16_t *)b)->topic;
    for (;; x++, y++) {
        int cx = *x == '/' ? 1 : (unsigned char)*x, cy = *y == '/' ? 1 : (unsigned char)*y;
        if (cx != cy || !cx) return cx - cy;
    }
}

/* What a row says about its payload in one line. */
static void summary(int slot, const aos_mqtt_topic_t *t, char *out, size_t n)
{
    char names[4][AOS_MQTT_FIELD_MAX];
    float last[4];
    switch (t->kind) {
    case AOS_MQTT_EMPTY: scpy(out, n, _("(vacío)")); return;
    case AOS_MQTT_BINARY: snprintf(out, n, _("binario · %u bytes"), (unsigned)t->len); return;
    case AOS_MQTT_NUMBER: fmt_num(out, n, strtod(t->payload, NULL)); return;
    case AOS_MQTT_JSON: {
        int nf = aos_mqtt_fields(slot, names, last, 4);
        if (nf > 0 && strncmp(t->topic, "homeassistant/", 14)) {
            size_t k = 0;
            out[0] = 0;
            for (int i = 0; i < nf && k + 8 < n; i++) {
                char v[24];
                fmt_num(v, sizeof v, last[i]);
                const char *nm = strrchr(names[i], '.') ? strrchr(names[i], '.') + 1 : names[i];
                const char *parts[4] = { i ? " \xC2\xB7 " : "", nm, " ", v };
                for (int q = 0; q < 4; q++) { scpy(out + k, n - k, parts[q]); k = strlen(out); }
            }
            return;
        }
        break;
    }
    default: break;
    }
    /* the text itself, on one line */
    size_t k = 0;
    for (const char *p = t->payload; *p && k + 1 < n; p++) out[k++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
    out[k] = 0;
}

static int add_row(void)
{
    if (s_nvr >= MAX_ROWS) { s_more++; return -1; }
    vrow_t *r = &s_vr[s_nvr];
    memset(r, 0, sizeof *r);
    r->slot = -1;
    return s_nvr++;
}

static void leaf_row(int slot, const aos_mqtt_topic_t *t, const char *name, int depth)
{
    int i = add_row();
    if (i < 0) return;
    vrow_t *r = &s_vr[i];
    r->slot = (int16_t)slot;
    r->key = fnv(t->topic, strlen(t->topic)) & ~1u;
    r->depth = (uint8_t)depth;
    r->seq = t->seq;
    r->count = t->count;
    r->kind = t->kind;
    r->retained = t->retained;
    r->sel = S.sel[0] && !strcmp(t->topic, S.sel);
    scpy(r->name, sizeof r->name, name[0] ? name : "\xC2\xB7");     /* an empty level: a middle dot */
    summary(slot, t, r->sum, sizeof r->sum);
}

/* The rows the tree shows now, from the service's table. */
static uint32_t model_build(void)
{
    s_nvr = s_more = 0;
    aos_mqtt_lock();
    int n = 0;
    for (int i = 0; i < AOS_MQTT_MAX_TOPICS; i++) {
        const aos_mqtt_topic_t *t = aos_mqtt_topic_at(i);
        if (!t || (S.filter[0] && !ci_contains(t->topic, S.filter))) continue;
        s_order[n++] = (int16_t)i;
    }
    qsort(s_order, (size_t)n, sizeof s_order[0], cmp_topic);
    if (S.filter[0]) {
        for (int k = 0; k < n; k++) leaf_row(s_order[k], aos_mqtt_topic_at(s_order[k]), aos_mqtt_topic_at(s_order[k])->topic, 0);
    } else {
        int srow[MAX_DEPTH];
        bool vis[MAX_DEPTH], opn[MAX_DEPTH];
        size_t pend[MAX_DEPTH];
        const char *prev = NULL;
        int pk = 0;
        for (int k = 0; k < n; k++) {
            const aos_mqtt_topic_t *t = aos_mqtt_topic_at(s_order[k]);
            const char *tp = t->topic;
            size_t end[MAX_DEPTH];
            int lv = 0;
            for (size_t p = 0;; p++) {
                if (!tp[p]) { end[lv++] = p; break; }
                if (tp[p] == '/' && lv < MAX_DEPTH - 1) end[lv++] = p;
            }
            int common = 0;
            if (prev)
                while (common < lv - 1 && common < pk - 1 && end[common] == pend[common] && !memcmp(tp, prev, end[common])) common++;
            for (int d = common; d < lv - 1; d++) {
                bool v = d == 0 || (vis[d - 1] && opn[d - 1] && srow[d - 1] >= 0);
                uint32_t key = fnv(tp, end[d]) | 1u;
                vis[d] = v;
                opn[d] = v && expanded(key);
                srow[d] = -1;
                if (!v) continue;
                int i = add_row();
                if (i < 0) continue;
                vrow_t *r = &s_vr[i];
                r->node = true;
                r->key = key;
                r->depth = (uint8_t)d;
                r->open = opn[d];
                size_t st = d ? end[d - 1] + 1 : 0;
                size_t len = end[d] - st;
                if (len >= sizeof r->name) len = sizeof r->name - 1;
                memcpy(r->name, tp + st, len);
                r->name[len] = 0;
                if (!len) scpy(r->name, sizeof r->name, "\xC2\xB7");
                srow[d] = i;
            }
            for (int d = 0; d < lv - 1; d++) {
                if (srow[d] < 0) continue;
                vrow_t *r = &s_vr[srow[d]];
                r->ntop++;
                r->msgs += t->count;
                if ((int32_t)(t->seq - r->seq) > 0) r->seq = t->seq;
            }
            bool leaf_vis = lv == 1 || (vis[lv - 2] && opn[lv - 2] && srow[lv - 2] >= 0);
            if (leaf_vis) leaf_row(s_order[k], t, tp + (lv > 1 ? end[lv - 2] + 1 : 0), lv - 1);
            prev = tp;
            pk = lv;
            memcpy(pend, end, sizeof pend);
        }
    }
    aos_mqtt_unlock();
    uint32_t sig = 2166136261u ^ (uint32_t)s_more * 7919u ^ (S.filter[0] ? 0x55u : 0);
    for (int i = 0; i < s_nvr; i++) sig = (sig ^ s_vr[i].key ^ (uint32_t)(s_vr[i].open << 7)) * 16777619u;
    return sig ^ (uint32_t)s_nvr;
}

/* -------------------------------------------------------------------------- */
/* Explorar                                                                    */
/* -------------------------------------------------------------------------- */

static void sheet_open(void);
static void detail_build(lv_obj_t *parent, int32_t w, int32_t h, bool in_sheet);

static void row_paint(urow_t *u, uint32_t now)
{
    lv_color_t base = u->selected ? AOS_C_CARD2 : AOS_C_CARD;
    if (u->flash_ms) {
        uint32_t el = now - u->flash_ms;
        if (el >= FLASH_MS) u->flash_ms = 0;
        else {
            lv_opa_t mix = (lv_opa_t)(110 * (FLASH_MS - el) / FLASH_MS);
            lv_obj_set_style_bg_color(u->o, lv_color_mix(C_MQ_D, base, mix), 0);
            lv_obj_set_style_bg_opa(u->o, LV_OPA_COVER, 0);
            return;
        }
    }
    lv_obj_set_style_bg_color(u->o, base, 0);
    lv_obj_set_style_bg_opa(u->o, u->selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nvr || i >= U.nrows) return;
    const vrow_t *r = &s_vr[i];
    if (r->node) {
        set_expanded(r->key, !r->open);
        exp_refresh(true);
        return;
    }
    aos_mqtt_lock();
    const aos_mqtt_topic_t *t = aos_mqtt_topic_at(r->slot);
    if (t) scpy(S.sel, sizeof S.sel, t->topic);
    aos_mqtt_unlock();
    if (!t) return;
    S.sel_field[0] = 0;
    U.d_seq = 0;
    if (U.land) {
        for (int k = 0; k < U.nrows; k++) {
            bool on = !U.rows[k].node && k == i;
            if (U.rows[k].selected != on) { U.rows[k].selected = on; row_paint(&U.rows[k], (uint32_t)aos_hal_uptime_ms()); }
        }
        lv_obj_clean(U.dpanel);
        detail_build(U.dpanel, lv_obj_get_content_width(U.dpanel), lv_obj_get_content_height(U.dpanel), false);
    } else {
        S.sheet = true;
        sheet_open();
    }
}

static void row_texts(urow_t *u, const vrow_t *r)
{
    char t[48], c[24];
    if (r->node) {
        snprintf(t, sizeof t, r->ntop == 1 ? _("%u tópico") : _("%u tópicos"), (unsigned)r->ntop);
        lv_label_set_text(u->count, t);
        return;
    }
    lv_label_set_text(u->sub, r->sum);
    fmt_int(c, sizeof c, r->count);
    snprintf(t, sizeof t, "\xC3\x97%s", c);                 /* x123 */
    lv_label_set_text(u->count, t);
    if (u->pin) {
        if (r->retained) lv_obj_remove_flag(u->pin, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(u->pin, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rows_rebuild(void)
{
    int32_t y = lv_obj_get_scroll_y(U.list);
    lv_obj_clean(U.list);
    U.nrows = 0;
    U.more = NULL;
    int32_t w = lv_obj_get_content_width(U.list);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < s_nvr; i++) {
        const vrow_t *r = &s_vr[i];
        urow_t *u = &U.rows[U.nrows++];
        memset(u, 0, sizeof *u);
        u->key = r->key;
        u->seq = r->seq;
        u->node = r->node;
        int32_t h = r->node ? NODE_H : LEAF_H;
        u->o = box(U.list, w, h);
        lv_obj_add_flag(u->o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(u->o, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(u->o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(u->o, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i) {
            lv_obj_t *line = box(u->o, w - 22 - r->depth * 34, 1);
            lv_obj_set_style_bg_color(line, AOS_C_CARD2, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_align(line, LV_ALIGN_TOP_RIGHT, 0, 0);
        }
        int32_t x = 22 + r->depth * 34;
        int32_t cw = 150;                           /* the count on the right */
        if (r->node) {
            lv_obj_t *ch = aos_label(u->o, r->open ? AOS_SYM_CHEVRON_DOWN : AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, r->open ? C_MQ : AOS_C_DIM);
            lv_obj_align(ch, LV_ALIGN_LEFT_MID, x - 4, 0);
            u->name = aos_label(u->o, r->name, aos_font_body, AOS_C_TEXT);
            dots(u->name);
            lv_obj_set_width(u->name, w - x - 40 - cw - 20);
            lv_obj_align(u->name, LV_ALIGN_LEFT_MID, x + 36, 0);
            u->count = aos_label(u->o, "", aos_font_small, AOS_C_DIM);
            lv_obj_align(u->count, LV_ALIGN_RIGHT_MID, -22, 0);
        } else {
            u->dot = box(u->o, 14, 14);
            lv_obj_set_style_radius(u->dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(u->dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(u->dot, r->kind == AOS_MQTT_NUMBER ? C_MQ : r->kind == AOS_MQTT_JSON ? C_JSON : lv_color_hex(0x636366), 0);
            lv_obj_align(u->dot, LV_ALIGN_TOP_LEFT, x + 4, 25);
            u->name = aos_label(u->o, r->name, aos_font_body, AOS_C_TEXT);
            dots(u->name);
            lv_obj_set_width(u->name, w - x - 36 - cw - 20);
            lv_obj_align(u->name, LV_ALIGN_TOP_LEFT, x + 34, 12);
            u->sub = lv_label_create(u->o);
            lv_obj_set_style_text_font(u->sub, &aos_mono_18, 0);
            lv_obj_set_style_text_color(u->sub, AOS_C_DIM, 0);
            dots(u->sub);
            lv_obj_set_width(u->sub, w - x - 34 - 22);
            lv_obj_align(u->sub, LV_ALIGN_BOTTOM_LEFT, x + 34, -14);
            u->count = aos_label(u->o, "", aos_font_caption, AOS_C_DIM);
            lv_obj_align(u->count, LV_ALIGN_TOP_RIGHT, -22, 18);
            u->pin = aos_label(u->o, AOS_SYM_PIN, &aos_sym_28, C_MQ);
            lv_obj_align(u->pin, LV_ALIGN_TOP_RIGHT, -22 - 96, 12);
            u->selected = U.land && r->sel;
        }
        aos_make_decorative(u->name);
        row_texts(u, r);
        row_paint(u, now);
    }
    if (s_more) {
        char t[64];
        snprintf(t, sizeof t, _("y %d más: usá la búsqueda"), s_more);
        U.more = box(U.list, w, 72);
        lv_obj_center(aos_label(U.more, t, aos_font_small, AOS_C_DIM));
    }
    if (!s_nvr) {
        aos_mqtt_state_t st = aos_mqtt_state();
        lv_obj_t *e = box(U.list, w, lv_obj_get_content_height(U.list));
        lv_obj_set_flex_flow(e, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(e, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(e, 14, 0);
        lv_obj_set_style_pad_hor(e, 40, 0);
        aos_label(e, S.filter[0] ? AOS_SYM_MAGNIFY : AOS_SYM_ACCESS_POINT, &aos_sym_44, S.filter[0] ? AOS_C_DIM : C_MQ);
        const char *title = S.filter[0] ? _("Ningún tópico coincide")
                          : st == AOS_MQTT_CONNECTED ? _("Esperando mensajes…")
                          : st == AOS_MQTT_UNCONFIGURED ? _("Conectá un broker") : _("Sin mensajes todavía");
        aos_label(e, title, aos_font_title, AOS_C_TEXT);
        const char *why = S.filter[0] ? _("Probá con otra parte del nombre.")
                        : st == AOS_MQTT_CONNECTED ? _("Suscripto; los tópicos aparecen acá a medida que llegan.")
                        : st == AOS_MQTT_UNCONFIGURED ? _("Poné la dirección en Conexión, o un mqtt.txt en la raíz de la tarjeta.")
                        : aos_mqtt_error()[0] ? aos_mqtt_error() : _("Cuando haya conexión, los tópicos aparecen acá.");
        lv_obj_t *l = aos_label(e, why, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        aos_make_decorative(e);
    }
    lv_obj_update_layout(U.list);
    lv_obj_scroll_to_y(U.list, y, LV_ANIM_OFF);
}

static void rows_update(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < U.nrows && i < s_nvr; i++) {
        urow_t *u = &U.rows[i];
        const vrow_t *r = &s_vr[i];
        if (r->seq != u->seq) {
            u->seq = r->seq;
            u->flash_ms = now ? now : 1;
            row_texts(u, r);
        } else if (r->node) {
            row_texts(u, r);
        }
    }
}

static void header_refresh(void)
{
    if (!U.st_text) return;
    const char *t;
    lv_color_t c;
    aos_mqtt_state_t st = aos_mqtt_state();
    state_look(st, &t, &c);
    lv_label_set_text(U.st_text, t);
    lv_obj_set_style_bg_color(U.st_dot, c, 0);
    if (!U.hdr_sub) return;
    aos_mqtt_config_t cfg;
    aos_mqtt_config(&cfg);
    aos_mqtt_stats_t s;
    aos_mqtt_stats(&s);
    char n[16], r[16];
    fmt_int(n, sizeof n, (uint32_t)s.topics);
    fmt_num(r, sizeof r, S.rate < 10 ? round(S.rate * 10) / 10 : round(S.rate));
    if (st == AOS_MQTT_CONNECTED)
        lv_label_set_text_fmt(U.hdr_sub, _("%s · %s tópicos · %s msg/s"), cfg.host, n, r);
    else if (!cfg.host[0])
        lv_label_set_text(U.hdr_sub, _("Sin broker configurado"));
    else
        lv_label_set_text_fmt(U.hdr_sub, "%s:%d · %s", cfg.host, cfg.port, aos_mqtt_error()[0] ? aos_mqtt_error() : t);
}

static void exp_refresh(bool force)
{
    if (!U.list) return;
    uint32_t sig = model_build();
    if (force || sig != U.sig) { U.sig = sig; rows_rebuild(); }
    else rows_update();
    header_refresh();
}

static void search_done(const char *v)
{
    scpy(S.filter, sizeof S.filter, v);
    build_page();
}

static void search_cb(lv_event_t *e) { text_entry(_("Buscar tópicos"), "zigbee2mqtt", S.filter, 60, false, search_done); }

static void search_clear_cb(lv_event_t *e)
{
    S.filter[0] = 0;
    build_page();
}

static void fold_cb(lv_event_t *e)
{
    if (S.nexp) { S.nexp = 0; build_page(); return; }
    /* unfold every level there is now (up to what is remembered) */
    aos_mqtt_lock();
    for (int i = 0; i < AOS_MQTT_MAX_TOPICS && S.nexp < MAX_EXP; i++) {
        const aos_mqtt_topic_t *t = aos_mqtt_topic_at(i);
        if (!t) continue;
        for (const char *p = t->topic; *p && S.nexp < MAX_EXP; p++)
            if (*p == '/') set_expanded(fnv(t->topic, (size_t)(p - t->topic)) | 1u, true);
    }
    aos_mqtt_unlock();
    build_page();
}

static void status_pill(lv_obj_t *parent)
{
    U.st_pill = box(parent, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_radius(U.st_pill, 26, 0);
    lv_obj_set_style_bg_color(U.st_pill, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.st_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(U.st_pill, 18, 0);
    lv_obj_set_flex_flow(U.st_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.st_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.st_pill, 10, 0);
    U.st_dot = box(U.st_pill, 14, 14);
    lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
    U.st_text = aos_label(U.st_pill, "", aos_font_caption, AOS_C_TEXT);
}

static void to_con_cb(lv_event_t *e);

static void build_exp(void)
{
    int32_t ch = lv_obj_get_content_height(U.content);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    int32_t lw = U.land ? 580 : w;
    lv_obj_t *left = column(U.content, lw, ch, false);
    if (!U.land) {
        lv_obj_t *head = box(left, lw, 96);
        lv_obj_align(aos_label(head, "MQTT", aos_font_large, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, 4, 0);
        U.hdr_sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
        dots(U.hdr_sub);
        lv_obj_set_width(U.hdr_sub, lw - 12);
        lv_obj_align(U.hdr_sub, LV_ALIGN_BOTTOM_LEFT, 6, 0);
        status_pill(head);
        lv_obj_add_flag(U.st_pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(U.st_pill, to_con_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_align(U.st_pill, LV_ALIGN_TOP_RIGHT, 0, 6);
    }

    /* the search and the fold button */
    lv_obj_t *sr = box(left, lw, 76);
    lv_obj_t *sb = box(sr, lw - 76 - 14, 76);
    lv_obj_set_style_radius(sb, 38, 0);
    lv_obj_set_style_bg_color(sb, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sb, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(sb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sb, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(aos_label(sb, AOS_SYM_MAGNIFY, &aos_sym_28, AOS_C_DIM), LV_ALIGN_LEFT_MID, 24, 0);
    U.search_l = aos_label(sb, S.filter[0] ? S.filter : _("Buscar tópicos"), aos_font_body, S.filter[0] ? AOS_C_TEXT : AOS_C_DIM);
    dots(U.search_l);
    lv_obj_set_width(U.search_l, lw - 76 - 14 - 76 - 90);
    lv_obj_align(U.search_l, LV_ALIGN_LEFT_MID, 68, 0);
    aos_make_decorative(U.search_l);
    if (S.filter[0]) {
        U.search_x = box(sb, 60, 60);
        lv_obj_set_style_radius(U.search_x, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(U.search_x, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(U.search_x, LV_OPA_COVER, 0);
        lv_obj_add_flag(U.search_x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(U.search_x, search_clear_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(aos_label(U.search_x, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT));
        lv_obj_align(U.search_x, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    lv_obj_t *fb = round_btn(sr, S.nexp ? AOS_SYM_CHEVRON_UP : AOS_SYM_ARROW_EXPAND_VERTICAL, fold_cb, NULL);
    lv_obj_align(fb, LV_ALIGN_RIGHT_MID, 0, 0);
    if (S.filter[0]) { lv_obj_add_state(fb, LV_STATE_DISABLED); lv_obj_set_style_opa(fb, LV_OPA_40, 0); }
    if (U.land) {
        /* no large title in landscape: the state on one line, under the search */
        lv_obj_t *head = box(left, lw, 30);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 10, 0);
        lv_obj_set_style_pad_left(head, 12, 0);
        lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(head, to_con_cb, LV_EVENT_CLICKED, NULL);
        U.st_dot = box(head, 12, 12);
        lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
        U.st_text = aos_label(head, "", aos_font_caption, AOS_C_TEXT);
        U.hdr_sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
        dots(U.hdr_sub);
        lv_obj_set_flex_grow(U.hdr_sub, 1);
        aos_make_decorative(head);
        lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_row(left, 12, 0);
    }

    /* the tree */
    U.list = card(left, lw, 100);
    lv_obj_set_flex_grow(U.list, 1);
    lv_obj_set_style_clip_corner(U.list, true, 0);
    lv_obj_add_flag(U.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(U.list, 8, 0);
    lv_obj_set_style_pad_bottom(left, 16, 0);
    lv_obj_update_layout(left);

    if (U.land) {
        int32_t rw = w - lw - AOS_UI_PAD;
        U.dpanel = card(U.content, rw, ch - 16);
        lv_obj_set_style_bg_color(U.dpanel, C_SHEET, 0);
        lv_obj_set_x(U.dpanel, lw + AOS_UI_PAD);
        lv_obj_set_style_pad_all(U.dpanel, 26, 0);
        lv_obj_update_layout(U.dpanel);
        detail_build(U.dpanel, lv_obj_get_content_width(U.dpanel), lv_obj_get_content_height(U.dpanel), false);
    }
    U.sig = 0;
    exp_refresh(true);
}

/* -------------------------------------------------------------------------- */
/* The detail of a topic: a sheet in portrait, a panel in landscape            */
/* -------------------------------------------------------------------------- */

static void pub_here_cb(lv_event_t *e);

static void field_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char names[AOS_MQTT_FIELDS][AOS_MQTT_FIELD_MAX];
    aos_mqtt_lock();
    int slot = aos_mqtt_topic_find(S.sel);
    int nf = slot >= 0 ? aos_mqtt_fields(slot, names, NULL, AOS_MQTT_FIELDS) : 0;
    aos_mqtt_unlock();
    if (i >= nf) return;
    scpy(S.sel_field, sizeof S.sel_field, names[i]);
    U.d_nf = -1;                    /* the chips again, with the new one lit */
    detail_refresh(true);
}

static void sheet_close_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    sheet_close();
}

/* The sheet's title: the last level, and when that one says little
 * ("state", "config", "SENSOR") the nearest one that says more before it:
 * "kitchen_plug · config", "plug1 · SENSOR", "temperature · state". */
static bool generic_level(const char *s, size_t n)
{
    static const char *const G[] = { "config", "state", "set", "command", "status", "availability", "SENSOR", "STATE",
        "POWER", "LWT", "RESULT", "get", "sensor", "binary_sensor", "switch", "light", "fan", "cover", "climate",
        "button", "number", "select", "text", "lock" };
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++) if (strlen(G[i]) == n && !strncmp(G[i], s, n)) return true;
    return false;
}

static void detail_title(const char *t, char *out, size_t n)
{
    const char *end = t + strlen(t), *ls = strrchr(t, '/');
    ls = ls ? ls + 1 : t;
    if (!*ls || !generic_level(ls, (size_t)(end - ls)) || ls == t) { scpy(out, n, *ls ? ls : t); return; }
    const char *e = ls - 1;                     /* the '/' before the last level */
    while (e > t) {
        const char *b = e;
        while (b > t && b[-1] != '/') b--;
        if (!generic_level(b, (size_t)(e - b)) && e > b) {
            size_t k = (size_t)(e - b) < 60 ? (size_t)(e - b) : 60;
            if (k + 5 > n) k = n > 5 ? n - 5 : 0;
            memcpy(out, b, k);
            memcpy(out + k, " \xC2\xB7 ", 5);
            out[k + 4] = 0;
            size_t used = k + 4;
            scpy(out + used, n - used, ls);
            return;
        }
        e = b - 1;
    }
    scpy(out, n, ls);
}

static void detail_build(lv_obj_t *parent, int32_t w, int32_t h, bool in_sheet)
{
    U.dcol = U.d_facts = U.d_fields = U.d_value = U.d_chart = U.d_payload = U.d_numwrap = U.d_empty = NULL;
    U.d_seq = 0;
    U.d_nf = -1;
    aos_mqtt_lock();
    int slot = S.sel[0] ? aos_mqtt_topic_find(S.sel) : -1;
    aos_mqtt_unlock();
    if (slot < 0) {
        U.d_empty = box(parent, w, h);
        lv_obj_set_flex_flow(U.d_empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(U.d_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(U.d_empty, 14, 0);
        aos_label(U.d_empty, AOS_SYM_MESSAGE_TEXT_OUTLINE, &aos_sym_44, AOS_C_DIM);
        aos_label(U.d_empty, _("Elegí un tópico"), aos_font_title, AOS_C_TEXT);
        lv_obj_t *l = aos_label(U.d_empty, _("Su último mensaje, cuándo llegó y un gráfico de cada número que trae."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, w - 80);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        S.sel[0] = 0;
        return;
    }
    char last[AOS_MQTT_TOPIC_MAX];
    detail_title(S.sel, last, sizeof last);

    /* the header: disc, name, the whole path; the close button in the sheet */
    lv_obj_t *head = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_t *disc = box(head, 72, 72);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, C_MQ_D, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_center(aos_label(disc, AOS_SYM_POUND, &aos_sym_44, lv_color_white()));
    int32_t tw = w - 96 - (in_sheet ? 90 : 0);
    lv_obj_t *nm = aos_label(head, last, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(nm, tw);
    dots(nm);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 92, 0);
    lv_obj_t *path = lv_label_create(head);
    lv_obj_set_style_text_font(path, &aos_mono_18, 0);
    lv_obj_set_style_text_color(path, AOS_C_DIM, 0);
    lv_label_set_text(path, S.sel);
    lv_label_set_long_mode(path, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(path, tw);
    lv_obj_align(path, LV_ALIGN_TOP_LEFT, 92, 46);
    if (in_sheet) {
        lv_obj_t *x = box(head, 72, 72);
        lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(x, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
        lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(x, sheet_close_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_center(aos_label(x, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT));
        lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    lv_obj_update_layout(head);

    /* the rest scrolls */
    int32_t hh = lv_obj_get_height(head);
    U.dcol = column(parent, w, h - hh - 20, true);
    lv_obj_set_y(U.dcol, hh + 20);
    lv_obj_set_style_pad_row(U.dcol, 18, 0);
    U.d_facts = chips_row(U.dcol, w);
    U.d_numwrap = box(U.dcol, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.d_numwrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.d_numwrap, 14, 0);
    /* the fields: one row that scrolls sideways, however many there are */
    U.d_fields = box(U.d_numwrap, w, 60);
    lv_obj_set_flex_flow(U.d_fields, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(U.d_fields, 10, 0);
    lv_obj_add_flag(U.d_fields, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.d_fields, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(U.d_fields, LV_SCROLLBAR_MODE_OFF);
    /* in landscape the panel is short: the number smaller, the chart lower */
    int32_t fh = U.land && !in_sheet ? 166 : 300;
    U.d_value = aos_label(U.d_numwrap, "", U.land && !in_sheet ? aos_font_large : aos_font_huge, C_MQ);
    lv_obj_t *frame = box(U.d_numwrap, w, fh);
    U.d_hi = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.d_hi, LV_ALIGN_TOP_LEFT, 0, 0);
    U.d_lo = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.d_lo, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    U.d_span = aos_label(frame, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.d_span, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    U.d_chart = lv_chart_create(frame);
    lv_obj_set_size(U.d_chart, w, fh - 64);
    lv_obj_align(U.d_chart, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_bg_opa(U.d_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(U.d_chart, 0, 0);
    lv_obj_set_style_pad_all(U.d_chart, 0, 0);
    lv_obj_set_style_line_color(U.d_chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_div_line_count(U.d_chart, 3, 0);
    lv_chart_set_type(U.d_chart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_size(U.d_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.d_chart, 4, LV_PART_ITEMS);
    lv_obj_remove_flag(U.d_chart, LV_OBJ_FLAG_CLICKABLE);
    U.d_ser = lv_chart_add_series(U.d_chart, C_MQ, LV_CHART_AXIS_PRIMARY_Y);

    section(U.dcol, _("MENSAJE"));
    lv_obj_t *pc = card(U.dcol, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pc, AOS_C_CARD, 0);
    lv_obj_set_style_radius(pc, 20, 0);
    lv_obj_set_style_pad_all(pc, 20, 0);
    U.d_payload = lv_label_create(pc);
    lv_obj_set_style_text_font(U.d_payload, &aos_mono_18, 0);
    lv_obj_set_style_text_color(U.d_payload, AOS_C_TEXT, 0);
    lv_obj_set_style_text_line_space(U.d_payload, 4, 0);
    lv_label_set_long_mode(U.d_payload, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(U.d_payload, w - 40);
    lv_obj_t *act = box(U.dcol, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_top(act, 4, 0);
    lv_obj_set_style_pad_gap(act, 12, 0);
    char cmd[AOS_MQTT_TOPIC_MAX];
    command_topic(S.sel, cmd, sizeof cmd);
    if (cmd[0]) pill(act, AOS_SYM_TOGGLE_SWITCH, _("Mandar una orden"), C_MQ_D, pub_here_cb, (void *)1);
    pill(act, AOS_SYM_SEND, _("Publicar acá"), cmd[0] ? AOS_C_CARD2 : C_MQ_D, pub_here_cb, NULL);
    detail_refresh(true);
}

static void detail_refresh(bool force)
{
    if (!U.dcol || !S.sel[0]) return;
    aos_mqtt_topic_t *tq = &A->dt;
    float *v = A->dv;
    uint32_t *tm = A->dtm;
    char names[AOS_MQTT_FIELDS][AOS_MQTT_FIELD_MAX];
    float last[AOS_MQTT_FIELDS];
    aos_mqtt_lock();
    int slot = aos_mqtt_topic_find(S.sel);
    const aos_mqtt_topic_t *tp = slot >= 0 ? aos_mqtt_topic_at(slot) : NULL;
    if (tp) *tq = *tp;
    int nf = tp ? aos_mqtt_fields(slot, names, last, AOS_MQTT_FIELDS) : 0;
    int fi = 0;
    for (int i = 0; i < nf; i++) if (!strcmp(names[i], S.sel_field)) fi = i;
    int np = nf ? aos_mqtt_history(slot, names[fi], v, tm, AOS_MQTT_HIST) : 0;
    aos_mqtt_unlock();
    if (!tp) return;                        /* gone from the table (cleared): leave what is shown */
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    bool changed = force || tq->seq != U.d_seq;
    U.d_seq = tq->seq;

    /* the facts; the age ticks even with nothing new */
    lv_obj_clean(U.d_facts);
    if (tq->retained) tag(U.d_facts, _("Retenido"), C_MQ_D, lv_color_white());
    char b[48], c[24];
    snprintf(b, sizeof b, "QoS %u", (unsigned)tq->qos);
    tag(U.d_facts, b, AOS_C_CARD2, AOS_C_TEXT);
    fmt_int(c, sizeof c, tq->count);
    snprintf(b, sizeof b, tq->count == 1 ? _("%s mensaje") : _("%s mensajes"), c);
    tag(U.d_facts, b, AOS_C_CARD2, AOS_C_TEXT);
    fmt_age(b, sizeof b, now - tq->last_ms);
    if (tq->last_wall) {
        time_t w = (time_t)tq->last_wall;
        struct tm lt;
        localtime_r(&w, &lt);
        size_t k = strlen(b);
        snprintf(b + k, sizeof b - k, " · %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
    tag(U.d_facts, b, AOS_C_CARD2, AOS_C_TEXT);
    fmt_bytes(c, sizeof c, tq->len);
    tag(U.d_facts, c, AOS_C_CARD2, AOS_C_TEXT);
    if (!changed) return;

    /* the numbers */
    if (!nf) {
        lv_obj_add_flag(U.d_numwrap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(U.d_numwrap, LV_OBJ_FLAG_HIDDEN);
        if (nf != U.d_nf) {
            U.d_nf = nf;
            lv_obj_clean(U.d_fields);
            if (nf > 1 || names[0][0]) {
                for (int i = 0; i < nf; i++) {
                    const char *dot = strrchr(names[i], '.');       /* ENERGY.Power -> Power */
                    lv_obj_t *c = chip(U.d_fields, !names[i][0] ? _("valor") : dot && dot[1] ? dot + 1 : names[i], i == fi, field_cb, (void *)(intptr_t)i);
                    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
                }
                lv_obj_update_layout(U.d_fields);
                lv_obj_scroll_to_view(lv_obj_get_child(U.d_fields, fi), LV_ANIM_OFF);
                lv_obj_remove_flag(U.d_fields, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(U.d_fields, LV_OBJ_FLAG_HIDDEN);
            }
        }
        fmt_num(b, sizeof b, last[fi]);
        lv_label_set_text(U.d_value, b);
        float lo = INFINITY, hi = -INFINITY;
        for (int i = 0; i < np; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
        if (np < 2) {
            lv_chart_set_point_count(U.d_chart, 2);
            lv_chart_set_all_values(U.d_chart, U.d_ser, LV_CHART_POINT_NONE);
            lv_label_set_text(U.d_hi, _("Hace falta más de un valor para el gráfico."));
            lv_label_set_text(U.d_lo, "");
            lv_label_set_text(U.d_span, "");
        } else {
            double scale = fmax(fabs(lo), fabs(hi)) < 1e7 ? 100.0 : 1.0;
            float pad = (hi - lo) * 0.12f;
            if (pad <= 0) pad = fabsf(hi) * 0.05f + 0.5f;
            lv_chart_set_point_count(U.d_chart, (uint32_t)np);
            lv_chart_set_axis_range(U.d_chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)lround((lo - pad) * scale), (int32_t)lround((hi + pad) * scale));
            for (int i = 0; i < np; i++) lv_chart_set_value_by_id(U.d_chart, U.d_ser, (uint32_t)i, (int32_t)lround(v[i] * scale));
            lv_chart_refresh(U.d_chart);
            fmt_num(c, sizeof c, hi);
            lv_label_set_text_fmt(U.d_hi, "%s %s", _("máx"), c);
            fmt_num(c, sizeof c, lo);
            lv_label_set_text_fmt(U.d_lo, "%s %s", _("mín"), c);
            fmt_age(c, sizeof c, now - tm[0]);
            lv_label_set_text_fmt(U.d_span, _("%d valores · %s"), np, c);
        }
    }

    /* the payload */
    if (tq->kind == AOS_MQTT_EMPTY) scpy(s_pretty, sizeof s_pretty, _("(vacío)"));
    else if (tq->kind == AOS_MQTT_JSON) aos_mqtt_pretty(tq->payload, s_pretty, sizeof s_pretty - 64);
    else scpy(s_pretty, sizeof s_pretty - 64, tq->payload);
    if (tq->len > strlen(tq->payload) && tq->kind != AOS_MQTT_EMPTY) {
        char tot[24];
        fmt_bytes(tot, sizeof tot, tq->len);
        size_t k = strlen(s_pretty);
        snprintf(s_pretty + k, sizeof s_pretty - k, _("\n… (recortado: %s en total)"), tot);
    }
    lv_label_set_text(U.d_payload, s_pretty);
}

static void sheet_close(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = NULL;
    U.dcol = NULL;
    S.sheet = false;
}

static void sheet_open(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_70, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.sheet, sheet_close_cb, LV_EVENT_CLICKED, NULL);
    int32_t h = U.H * 82 / 100;
    lv_obj_t *c = box(U.sheet, U.W, h);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, C_SHEET, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS + 8, 0);
    lv_obj_set_style_pad_hor(c, 28, 0);
    lv_obj_set_style_pad_top(c, 34, 0);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, AOS_UI_RADIUS + 8);    /* the bottom corners off the screen */
    /* the grabber */
    lv_obj_t *g = box(c, 72, 8);
    lv_obj_set_style_radius(g, 4, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(0x48484A), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, -20);
    lv_obj_t *in = box(c, U.W - 56, h - 34 - AOS_UI_RADIUS - 8);
    lv_obj_set_y(in, 0);
    detail_build(in, U.W - 56, h - 34 - AOS_UI_RADIUS - 8 - 10, true);
}

/* -------------------------------------------------------------------------- */
/* Publicar                                                                    */
/* -------------------------------------------------------------------------- */

static void pub_topic_done(const char *v) { scpy(S.pub_topic, sizeof S.pub_topic, v); pub_save(); build_page(); }
static void pub_payload_done(const char *v) { scpy(S.pub_payload, sizeof S.pub_payload, v); pub_save(); build_page(); }

static void pub_topic_cb(lv_event_t *e) { text_entry_ex(_("Tópico"), "casa/luz/set", S.pub_topic, AOS_MQTT_TOPIC_MAX - 1, false, true, pub_topic_done); }
static void pub_payload_cb(lv_event_t *e) { text_entry(_("Mensaje"), "{\"state\":\"ON\"}", S.pub_payload, AOS_MQTT_PUB_MAX, false, pub_payload_done); }

static void pub_qos_cb(lv_event_t *e)
{
    S.pub_qos = (int)(intptr_t)lv_event_get_user_data(e);
    pub_save();
    build_page();
}

static void pub_retain_cb(lv_event_t *e)
{
    S.pub_retain = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    pub_save();
}

static void do_publish(const char *topic, const char *payload, int qos, bool retain, bool remember)
{
    aos_mqtt_config_t cfg;
    aos_mqtt_config(&cfg);
    char msg[160];
    if (!topic[0]) { aos_ui_toast(_("Falta el tópico"), 2000); return; }
    if (strpbrk(topic, "+#")) { aos_ui_toast(_("Un tópico para publicar no lleva + ni #"), 2500); return; }
    if (!cfg.host[0]) { aos_ui_toast(_("Primero configurá el broker en Conexión"), 2500); return; }
    if (!cfg.enabled) { aos_ui_toast(_("MQTT está apagado en Conexión"), 2500); return; }
    if (!aos_mqtt_publish(topic, payload, qos, retain)) { aos_ui_toast(_("La cola de envío está llena"), 2500); return; }
    if (aos_mqtt_state() == AOS_MQTT_CONNECTED) snprintf(msg, sizeof msg, _("Publicado en %.100s"), topic);
    else scpy(msg, sizeof msg, _("En cola: sale cuando haya conexión"));
    aos_ui_toast(msg, 1800);
    if (remember) {
        fav_add(topic, payload, qos, retain);
        if (S.tab == TAB_PUB) build_page();
    }
}

static void pub_go_cb(lv_event_t *e) { do_publish(S.pub_topic, S.pub_payload, S.pub_qos, S.pub_retain, true); }

static void fav_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nfav) return;
    fav_t f = S.fav[i];
    do_publish(f.topic, f.payload, f.qos, f.retain, false);
}

static void fav_edit_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nfav) return;
    scpy(S.pub_topic, sizeof S.pub_topic, S.fav[i].topic);
    scpy(S.pub_payload, sizeof S.pub_payload, S.fav[i].payload);
    S.pub_qos = S.fav[i].qos;
    S.pub_retain = S.fav[i].retain;
    pub_save();
    build_page();
    aos_ui_toast(_("Cargado en el formulario"), 1500);
}

static void fav_pin_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nfav) return;
    fav_t f = S.fav[i];
    f.pinned = !f.pinned;
    memmove(S.fav + i, S.fav + i + 1, (size_t)(S.nfav - i - 1) * sizeof S.fav[0]);
    S.nfav--;
    int at = 0;
    if (!f.pinned) while (at < S.nfav && S.fav[at].pinned) at++;     /* first of the unpinned */
    memmove(S.fav + at + 1, S.fav + at, (size_t)(S.nfav - at) * sizeof S.fav[0]);
    S.fav[at] = f;
    S.nfav++;
    fav_save();
    build_page();
}

static void fav_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.nfav) return;
    memmove(S.fav + i, S.fav + i + 1, (size_t)(S.nfav - i - 1) * sizeof S.fav[0]);
    S.nfav--;
    fav_save();
    build_page();
}

static lv_obj_t *form_row(lv_obj_t *parent, int32_t w, const char *label, bool line)
{
    lv_obj_t *r = box(parent, w, U.land ? 84 : AOS_UI_ROW_H);
    lv_obj_set_style_pad_hor(r, 22, 0);
    if (line) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    lv_obj_align(aos_label(r, label, aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    return r;
}

static lv_obj_t *form_value_row(lv_obj_t *parent, int32_t w, const char *label, const char *value, const char *empty,
                                bool line, lv_event_cb_t cb)
{
    lv_obj_t *r = form_row(parent, w, label, line);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *v = lv_label_create(r);
    lv_obj_set_style_text_font(v, value[0] ? &aos_mono_22 : aos_font_body, 0);
    lv_obj_set_style_text_color(v, value[0] ? C_MQ : AOS_C_DIM, 0);
    lv_label_set_text(v, value[0] ? value : empty);
    dots(v);
    lv_obj_set_style_max_width(v, w - 44 - 190 - 40, 0);
    lv_obj_set_width(v, LV_SIZE_CONTENT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_align(aos_label(r, AOS_SYM_PENCIL, &aos_sym_28, AOS_C_DIM), LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static void build_pub(void)
{
    int32_t ch = lv_obj_get_content_height(U.content);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = 580;
        rw = w - lw - AOS_UI_PAD;
        left = column(U.content, lw, ch, true);
        right = column(U.content, rw, ch, true);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(U.content, w, ch, true);
    }
    lv_obj_t *head = box(left, lw, U.land ? 76 : 96);
    lv_obj_align(aos_label(head, _("Publicar"), U.land ? aos_font_title : aos_font_large, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, 4, 0);
    U.p_sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    dots(U.p_sub);
    lv_obj_set_width(U.p_sub, lw - 12);
    lv_obj_align(U.p_sub, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    status_pill(head);
    lv_obj_align(U.st_pill, LV_ALIGN_TOP_RIGHT, 0, 6);

    lv_obj_t *f = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_COLUMN);
    U.p_topic = form_value_row(f, lw, _("Tópico"), S.pub_topic, _("tocá para escribirlo"), false, pub_topic_cb);
    U.p_payload = form_value_row(f, lw, _("Mensaje"), S.pub_payload, _("vacío"), true, pub_payload_cb);
    lv_obj_t *q = form_row(f, lw, "QoS", true);
    lv_obj_t *qc = box(q, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(qc, 10, 0);
    chip(qc, "0", S.pub_qos == 0, pub_qos_cb, (void *)0);
    chip(qc, "1", S.pub_qos == 1, pub_qos_cb, (void *)1);
    lv_obj_align(qc, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *rr = form_row(f, lw, _("Retener"), true);
    lv_obj_t *sw = lv_switch_create(rr);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, C_MQ_D, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, S.pub_retain);
    lv_obj_add_event_cb(sw, pub_retain_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *go = box(left, lw, 100);
    lv_obj_set_style_radius(go, 50, 0);
    lv_obj_set_style_bg_color(go, C_MQ_D, 0);
    lv_obj_set_style_bg_opa(go, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(go, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(go, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(go, pub_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gr = box(go, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gr, 14, 0);
    lv_obj_center(gr);
    aos_label(gr, AOS_SYM_SEND, &aos_sym_44, lv_color_white());
    aos_label(gr, _("Publicar"), aos_font_title, lv_color_white());
    aos_make_decorative(gr);
    caption(left, S.pub_retain ? _("Retenido: el broker lo guarda y se lo da a cada uno que se suscriba después. Un mensaje vacío retenido lo borra.")
                               : _("QoS 1 espera la confirmación del broker (PUBACK) y reintenta si no llega."), lw);

    if (U.land) box(right, rw, 60);           /* level with the form */
    section(right, _("RECIENTES"));
    if (!S.nfav) {
        lv_obj_t *e = card(right, rw, 150);
        lv_obj_t *l = aos_label(e, _("Lo que publiques queda acá, para mandarlo de nuevo con un toque."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, rw - 60);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    } else {
        U.p_favs = card(right, rw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(U.p_favs, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_clip_corner(U.p_favs, true, 0);
        for (int i = 0; i < S.nfav; i++) {
            const fav_t *fv = &S.fav[i];
            lv_obj_t *r = box(U.p_favs, rw, 104);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_event_cb(r, fav_tap_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(r, fav_edit_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
            if (i) {
                lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
                lv_obj_set_style_border_width(r, 1, 0);
                lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
            }
            lv_obj_t *pin = box(r, 80, 104);
            lv_obj_add_flag(pin, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pin, fav_pin_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_center(aos_label(pin, fv->pinned ? AOS_SYM_PIN : AOS_SYM_PIN_OFF, &aos_sym_28, fv->pinned ? AOS_C_YELLOW : lv_color_hex(0x48484A)));
            lv_obj_align(pin, LV_ALIGN_LEFT_MID, 0, 0);
            int32_t tw = rw - 80 - 170;
            lv_obj_t *tl = aos_label(r, fv->topic, aos_font_body, AOS_C_TEXT);
            dots(tl);
            lv_obj_set_width(tl, tw);
            lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 80, 14);
            char one[FAV_PAYLOAD];
            scpy(one, sizeof one, fv->payload[0] ? fv->payload : _("(vacío)"));
            for (char *p = one; *p; p++) if (*p == '\n') *p = ' ';
            lv_obj_t *pl = lv_label_create(r);
            lv_obj_set_style_text_font(pl, &aos_mono_18, 0);
            lv_obj_set_style_text_color(pl, AOS_C_DIM, 0);
            lv_label_set_text(pl, one);
            dots(pl);
            lv_obj_set_width(pl, tw);
            lv_obj_align(pl, LV_ALIGN_BOTTOM_LEFT, 80, -16);
            char tg[24];
            snprintf(tg, sizeof tg, "QoS %u%s", (unsigned)fv->qos, fv->retain ? " · R" : "");
            lv_obj_align(aos_label(r, tg, aos_font_caption, AOS_C_DIM), LV_ALIGN_RIGHT_MID, -96, 0);
            lv_obj_t *del = box(r, 80, 104);
            lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(del, fav_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_center(aos_label(del, AOS_SYM_CLOSE, &aos_sym_28, lv_color_hex(0x636366)));
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
            aos_make_decorative(tl);
            aos_make_decorative(pl);
        }
        caption(right, _("Tocá uno para publicarlo otra vez; mantenelo apretado para cargarlo arriba. El alfiler lo deja fijo."), rw);
    }
    header_refresh();
}

/* "Publicar acá": the form with this topic and its payload. "Mandar una
 * orden": the topic the device listens on and a toggle in its dialect. */
static void pub_here_cb(lv_event_t *e)
{
    bool order = lv_event_get_user_data(e) != NULL;
    aos_mqtt_lock();
    int slot = aos_mqtt_topic_find(S.sel);
    const aos_mqtt_topic_t *t = slot >= 0 ? aos_mqtt_topic_at(slot) : NULL;
    if (t && order) {
        command_topic(t->topic, S.pub_topic, sizeof S.pub_topic);
        scpy(S.pub_payload, sizeof S.pub_payload, !strncmp(S.pub_topic, "zigbee2mqtt/", 12) ? "{\"state\":\"TOGGLE\"}" : "TOGGLE");
        S.pub_retain = false;
    } else if (t) {
        scpy(S.pub_topic, sizeof S.pub_topic, t->topic);
        if (t->kind != AOS_MQTT_BINARY && t->len == strlen(t->payload)) scpy(S.pub_payload, sizeof S.pub_payload, t->payload);
        else S.pub_payload[0] = 0;
        S.pub_retain = t->retained;
    }
    aos_mqtt_unlock();
    if (!t) return;
    pub_save();
    sheet_close();
    S.tab = TAB_PUB;
    aos_hal_pref_set_i32("mqtt_tab", S.tab);
    build_page();
}

/* -------------------------------------------------------------------------- */
/* Conexión                                                                    */
/* -------------------------------------------------------------------------- */

static void cfg_apply(aos_mqtt_config_t *c)
{
    aos_mqtt_set_config(c);
    build_page();
}

static void host_done(const char *v)
{
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    char h[80];
    scpy(h, sizeof h, v);
    char *colon = strrchr(h, ':');
    if (colon && atoi(colon + 1) > 0) { c.port = atoi(colon + 1); *colon = 0; }
    else if (colon && !colon[1]) *colon = 0;
    scpy(c.host, sizeof c.host, h);
    c.enabled = true;
    cfg_apply(&c);
}

static void user_done(const char *v) { aos_mqtt_config_t c; aos_mqtt_config(&c); scpy(c.user, sizeof c.user, v); cfg_apply(&c); }
static void pass_done(const char *v) { aos_mqtt_config_t c; aos_mqtt_config(&c); scpy(c.pass, sizeof c.pass, v); cfg_apply(&c); }
static void cid_done(const char *v) { aos_mqtt_config_t c; aos_mqtt_config(&c); scpy(c.client_id, sizeof c.client_id, v); cfg_apply(&c); }
static void sub_done(const char *v) { aos_mqtt_config_t c; aos_mqtt_config(&c); scpy(c.sub, sizeof c.sub, v[0] ? v : "#"); cfg_apply(&c); }

static void host_cb(lv_event_t *e)
{
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    char v[80] = "";
    if (c.host[0]) snprintf(v, sizeof v, "%s:%d", c.host, c.port);
    text_entry(_("Broker (dirección:puerto)"), "192.168.1.10:1883", v, 70, false, host_done);
}

static void user_cb(lv_event_t *e) { aos_mqtt_config_t c; aos_mqtt_config(&c); text_entry(_("Usuario"), _("sin usuario"), c.user, 46, false, user_done); }
static void pass_cb(lv_event_t *e) { text_entry(_("Contraseña"), _("sin contraseña"), "", 62, true, pass_done); }
static void cid_cb(lv_event_t *e) { aos_mqtt_config_t c; aos_mqtt_config(&c); text_entry(_("ID de cliente (vacío: automático)"), aos_mqtt_client_id(), c.client_id, 46, false, cid_done); }
static void sub_cb(lv_event_t *e) { aos_mqtt_config_t c; aos_mqtt_config(&c); text_entry(_("Suscripción (separá con comas)"), "#", c.sub, 126, false, sub_done); }

static void ka_cb(lv_event_t *e)
{
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    c.keepalive = (int)(intptr_t)lv_event_get_user_data(e);
    cfg_apply(&c);
}

static void enable_cb(lv_event_t *e)
{
    aos_mqtt_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void reconnect_cb(lv_event_t *e) { aos_mqtt_reconnect(); aos_ui_toast(_("Reconectando…"), 1500); }

static void import_cb(lv_event_t *e)
{
    bool got = aos_mqtt_import_file();
    aos_ui_toast(got ? _("Configuración leída de mqtt.txt") : _("No hay un mqtt.txt nuevo en la tarjeta"), 2200);
    build_page();
}

static void clear_cb(lv_event_t *e)
{
    aos_mqtt_clear();
    S.sel[0] = 0;
    aos_ui_toast(_("Tabla vacía: los retenidos vuelven al reconectar"), 2200);
}

static void to_con_cb(lv_event_t *e)
{
    S.tab = TAB_CON;
    aos_hal_pref_set_i32("mqtt_tab", S.tab);
    build_page();
}

static lv_obj_t *link_row(lv_obj_t *parent, int32_t w, const char *text, lv_color_t color, bool line, lv_event_cb_t cb)
{
    lv_obj_t *r = form_row(parent, w, "", line);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(aos_label(r, text, aos_font_body, color), LV_ALIGN_LEFT_MID, 0, 0);
    return r;
}

static void con_refresh(void)
{
    if (!U.c_state) return;
    const char *t;
    lv_color_t col;
    aos_mqtt_state_t st = aos_mqtt_state();
    state_look(st, &t, &col);
    lv_label_set_text(U.c_state, t);
    lv_obj_set_style_bg_color(U.c_dot, col, 0);
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    aos_mqtt_stats_t s;
    aos_mqtt_stats(&s);
    char d[160], a[32];
    if (st == AOS_MQTT_CONNECTED) {
        fmt_age(a, sizeof a, (uint32_t)aos_hal_uptime_ms() - s.connected_ms);
        snprintf(d, sizeof d, _("%s:%d · desde %s"), c.host, c.port, a);
        if (aos_mqtt_error()[0]) { size_t k = strlen(d); snprintf(d + k, sizeof d - k, " · %s", aos_mqtt_error()); }
    } else if (!c.host[0]) {
        snprintf(d, sizeof d, "%s", _("Poné la dirección del broker abajo, o un mqtt.txt en la tarjeta."));
    } else if (st == AOS_MQTT_OFF) {
        snprintf(d, sizeof d, "%s", _("Prendelo para conectar."));
    } else {
        snprintf(d, sizeof d, "%s:%d · %s", c.host, c.port, aos_mqtt_error()[0] ? aos_mqtt_error() : t);
    }
    lv_label_set_text(U.c_detail, d);
    if (lv_obj_has_state(U.c_sw, LV_STATE_CHECKED) != c.enabled) lv_obj_set_state(U.c_sw, LV_STATE_CHECKED, c.enabled);
    char v[24];
    fmt_int(v, sizeof v, s.rx_msgs); lv_label_set_text(U.c_val[0], v);
    fmt_int(v, sizeof v, s.tx_msgs); lv_label_set_text(U.c_val[1], v);
    fmt_int(v, sizeof v, (uint32_t)s.topics); lv_label_set_text(U.c_val[2], v);
    fmt_bytes(v, sizeof v, s.rx_bytes); lv_label_set_text(U.c_val[3], v);
    fmt_bytes(v, sizeof v, s.tx_bytes); lv_label_set_text(U.c_val[4], v);
    fmt_num(v, sizeof v, S.rate < 10 ? round(S.rate * 10) / 10 : round(S.rate)); lv_label_set_text(U.c_val[5], v);
    if (s.ping_ms >= 0 && st == AOS_MQTT_CONNECTED) lv_label_set_text_fmt(U.c_val[6], "%d ms", (int)s.ping_ms);
    else lv_label_set_text(U.c_val[6], "--");
    fmt_int(v, sizeof v, s.connects); lv_label_set_text(U.c_val[7], v);
    lv_label_set_text_fmt(U.c_val[8], "%u", (unsigned)(s.pending + s.queued));
    lv_obj_set_style_text_color(U.c_val[8], s.pending + s.queued ? AOS_C_ORANGE : AOS_C_TEXT, 0);
}

static void build_con(void)
{
    int32_t ch = lv_obj_get_content_height(U.content);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *left, *right;
    int32_t lw = w, rw = w;
    if (U.land) {
        lw = 620;
        rw = w - lw - AOS_UI_PAD;
        left = column(U.content, lw, ch, true);
        right = column(U.content, rw, ch, true);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
    } else {
        left = right = column(U.content, w, ch, true);
    }
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    lv_obj_t *head = box(left, lw, U.land ? 76 : 96);
    lv_obj_align(aos_label(head, _("Conexión"), U.land ? aos_font_title : aos_font_large, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_t *sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_text_fmt(sub, _("MQTT 3.1.1 · cliente %s"), aos_mqtt_client_id());
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 6, 0);

    /* the state, and the switch */
    lv_obj_t *sc = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sc, 24, 0);
    lv_obj_set_style_pad_right(sc, 140, 0);
    lv_obj_t *sr = box(sc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sr, 14, 0);
    U.c_dot = box(sr, 20, 20);
    lv_obj_set_style_radius(U.c_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.c_dot, LV_OPA_COVER, 0);
    U.c_state = aos_label(sr, "", aos_font_title, AOS_C_TEXT);
    U.c_detail = aos_label(sc, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(U.c_detail, lw - 24 - 140);
    lv_label_set_long_mode(U.c_detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_y(U.c_detail, 56);
    U.c_sw = lv_switch_create(sc);
    lv_obj_set_size(U.c_sw, 96, 52);
    lv_obj_add_flag(U.c_sw, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(U.c_sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(U.c_sw, LV_STATE_CHECKED, c.enabled);
    lv_obj_add_event_cb(U.c_sw, enable_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(U.c_sw, LV_ALIGN_RIGHT_MID, 116, 0);

    /* the settings */
    section(left, _("BROKER"));
    lv_obj_t *g = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    char hp[96] = "";
    if (c.host[0]) snprintf(hp, sizeof hp, "%s:%d", c.host, c.port);
    form_value_row(g, lw, _("Servidor"), hp, _("sin configurar"), false, host_cb);
    form_value_row(g, lw, _("Usuario"), c.user, _("ninguno"), true, user_cb);
    form_value_row(g, lw, _("Contraseña"), c.pass[0] ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : "", _("ninguna"), true, pass_cb);
    char cid[64];
    snprintf(cid, sizeof cid, "%s", c.client_id);
    form_value_row(g, lw, _("ID de cliente"), cid, _("automático"), true, cid_cb);
    form_value_row(g, lw, _("Suscripción"), c.sub, "#", true, sub_cb);
    lv_obj_t *kr = form_row(g, lw, "Keepalive", true);
    lv_obj_t *kc = box(kr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kc, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(kc, 10, 0);
    static const int KA[] = { 15, 30, 60, 120 };
    for (int i = 0; i < 4; i++) {
        char t[12];
        snprintf(t, sizeof t, "%d s", KA[i]);
        chip(kc, t, c.keepalive == KA[i], ka_cb, (void *)(intptr_t)KA[i]);
    }
    lv_obj_align(kc, LV_ALIGN_RIGHT_MID, 0, 0);

    /* the counters */
    if (U.land) box(right, rw, 60);
    section(right, _("CONTADORES"));
    lv_obj_t *fg = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(fg, 12, 0);
    static const char *const CN[9] = { N_("Recibidos"), N_("Enviados"), N_("Tópicos"), N_("Datos recibidos"), N_("Datos enviados"),
                                       N_("Mensajes/s"), "Ping", N_("Conexiones"), N_("Sin confirmar") };
    int32_t fw = (rw - 2 * 12) / 3;
    for (int i = 0; i < 9; i++) {
        lv_obj_t *k = card(fg, fw, 108);
        lv_obj_set_style_pad_all(k, 16, 0);
        lv_obj_set_style_radius(k, 22, 0);
        lv_obj_align(aos_label(k, aos_tr(CN[i]), aos_font_caption, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        U.c_val[i] = aos_label(k, "--", aos_font_title, AOS_C_TEXT);
        lv_obj_align(U.c_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 4);
    }
    lv_obj_t *ac = card(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ac, LV_FLEX_FLOW_COLUMN);
    link_row(ac, rw, _("Reconectar"), AOS_C_ACCENT, false, reconnect_cb);
    link_row(ac, rw, _("Leer mqtt.txt de la tarjeta"), AOS_C_ACCENT, true, import_cb);
    link_row(ac, rw, _("Vaciar la tabla de tópicos"), AOS_C_RED, true, clear_cb);
    caption(right, _("La contraseña es incómoda de tipear: poné un mqtt.txt en la raíz de la tarjeta con renglones host=, port=, user=, password=, client_id= y sub=. P4OS la guarda y la borra del archivo. Sin TLS: sólo brokers de la red local."), rw);
    con_refresh();
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    overlay_close();
    if (U.sheet) { lv_obj_delete(U.sheet); U.sheet = NULL; }
    lv_obj_clean(U.content);
    U.list = U.dpanel = U.dcol = U.st_text = U.st_dot = U.st_pill = U.hdr_sub = U.search_l = U.search_x = U.more = NULL;
    U.p_topic = U.p_payload = U.p_favs = U.p_sub = NULL;
    U.c_dot = U.c_state = U.c_detail = U.c_sw = NULL;
    U.nrows = 0;
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_MQ : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    if (S.tab == TAB_EXP) build_exp();
    else if (S.tab == TAB_PUB) build_pub();
    else build_con();
    if (S.tab == TAB_EXP && !U.land && S.sheet && S.sel[0]) sheet_open();
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    S.tab = t;
    if (t != TAB_EXP) S.sheet = false;
    aos_hal_pref_set_i32("mqtt_tab", S.tab);
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    /* the rate, once a second */
    if (now - S.rate_ms >= 1000) {
        aos_mqtt_stats_t s;
        aos_mqtt_stats(&s);
        if (S.rate_ms && s.rx_msgs >= S.rate_rx) {
            float r = (float)(s.rx_msgs - S.rate_rx) * 1000.0f / (float)(now - S.rate_ms);
            S.rate = S.rate * 0.5f + r * 0.5f;
        }
        S.rate_rx = s.rx_msgs;
        S.rate_ms = now;
    }
    uint32_t ver = aos_mqtt_version();
    if (S.tab == TAB_EXP && U.list) {
        if (ver != U.seen && now - U.t_exp >= 300) { U.seen = ver; U.t_exp = now; exp_refresh(false); }
        for (int i = 0; i < U.nrows; i++) if (U.rows[i].flash_ms) row_paint(&U.rows[i], now);
        if (U.dcol && now - U.t_det >= 500) { U.t_det = now; detail_refresh(false); }
    } else if (S.tab == TAB_PUB && now - U.t_con >= 500) {
        U.t_con = now;
        header_refresh();
        if (U.p_sub) {
            aos_mqtt_config_t c;
            aos_mqtt_config(&c);
            if (c.host[0]) lv_label_set_text_fmt(U.p_sub, _("a %s:%d como %s"), c.host, c.port, aos_mqtt_client_id());
            else lv_label_set_text(U.p_sub, _("Sin broker configurado"));
        }
    } else if (S.tab == TAB_CON && now - U.t_con >= 500) {
        U.t_con = now;
        con_refresh();
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    aos_mqtt_start();
    if (!A) A = big_calloc(sizeof *A);
    if (!A) {
        lv_obj_center(aos_label(root, _("No hay memoria para abrir MQTT."), aos_font_body, AOS_C_DIM));
        return NULL;
    }
    settings_load();
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
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
    if (!A) return;
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (!A) return false;
    if (U.overlay) { overlay_close(); return true; }
    if (U.sheet) { sheet_close(); return true; }
    if (S.tab == TAB_EXP && S.filter[0]) { S.filter[0] = 0; build_page(); return true; }
    return false;
}

void aos_app_mqtt_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.mqtt", .name = "MQTT", .icon = AOS_SYM_ACCESS_POINT,
            .color_a = 0xA78BFA, .color_b = 0x6D28D9,
            .order = 530,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
