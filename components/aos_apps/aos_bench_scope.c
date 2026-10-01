/*
 * P4OS - the Banco's oscilloscope tab: the Rigol DS1000Z of aos_scope.c as
 * a touch instrument.
 *
 *   upright                          lying down
 *   +----------------------------+   +------------------+-----------+
 *   | Run/Stop Única Auto Captura|   | Run/Stop ...     | CH1  CH2  |
 *   | +------------------------+ |   | +--------------+ | CH3  CH4  |
 *   | |      12 x 8 grid       | |   | |  12 x 8 grid | | channel   |
 *   | +------------------------+ |   | +--------------+ | card      |
 *   | [CH1 meas]   [CH2 meas]    |   | [CH1] [CH2]      | time and  |
 *   | [CH3 meas]   [CH4 meas]    |   | [CH3] [CH4]      | trigger   |
 *   | CH1  CH2  CH3  CH4         |   +------------------+-----------+
 *   | channel card               |
 *   | time and trigger card      |
 *   +----------------------------+
 *
 * The grid is one lv_canvas whose pixels this file writes itself (a fill,
 * the dotted divisions, and each trace as one vertical span per pixel
 * column, 2 px thick), redrawn only when a new set of traces arrives or
 * something that moves them changes: one image for LVGL to push, however
 * many channels and points. The labels over it (the trigger's state, the
 * timebase, the channels' zero markers, the trigger level) are LVGL
 * objects.
 *
 * The channel chips behave like the scope's own CH keys: a channel that is
 * off comes on and is chosen; one that is on but not chosen is chosen; the
 * chosen one goes off. The card under them acts on the chosen channel.
 * Dragging up and down on the grid moves the chosen channel.
 *
 * Nothing here waits for the scope: every knob queues a command
 * (aos_scope_cmd), which the service also writes into its status at once.
 */
#include "aos_bench.h"
#include "aos_scope.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NCH AOS_SCOPE_CHANNELS

/* The Rigol's own colours, a touch brighter for the panel. */
static const uint32_t CH_RGB[NCH] = { 0xFFD83B, 0x2EE6F0, 0xFF4FD8, 0x4F8CFF };
#define RGB_TRIG      0xFF9F0A
#define RGB_BG        0x050507
#define RGB_GRID      0x34343A
#define RGB_AXIS      0x5A5A62
#define C_RUN         lv_color_hex(0x1F8A3B)
#define C_STOP        lv_color_hex(0xB3261E)
#define C_BTN         lv_color_hex(0x2C2C2E)
#define MINUS         "\xE2\x88\x92"       /* U+2212 */
#define MU            "\xCE\xBC"           /* U+03BC, Inter has it */

static lv_color_t ch_color(int c) { return lv_color_hex(CH_RGB[c]); }

/* What outlives a rebuild (a rotation, a tab switch). */
static struct {
    int sel;                        /* the chosen channel, 0..3 */
    bool conn_open;                 /* the connection card, opened by hand while connected */
} P;

typedef struct {
    lv_obj_t *box, *cap, *val, *minus, *plus;
} stepper_t;

static struct {
    lv_obj_t *root;
    int32_t w, h;
    bool land;
    /* the grid */
    lv_obj_t *tracebox, *canvas;
    uint16_t *px;
    int32_t tw, th, stride, dx, dy;
    lv_obj_t *badge, *badge_l, *fps_l, *info_l, *gnd[NCH], *trig_tag;
    float *wave;                    /* NCH x AOS_SCOPE_POINTS, the tab's copy */
    int wave_n[NCH];
    uint32_t seen_seq;
    uint32_t drawn_key;
    uint32_t trig_changed_ms;       /* the level's dashed line shows a while after a change */
    float last_level;
    /* the connection card */
    lv_obj_t *conn, *conn_status, *conn_host, *conn_go, *conn_go_l, *conn_close;
    /* the controls */
    lv_obj_t *controls[3];          /* dimmed while there is no scope */
    lv_obj_t *run_btn, *run_g, *run_l, *link_chip, *link_l, *link_g;
    lv_obj_t *chip[NCH], *chip_name[NCH], *chip_val[NCH];
    lv_obj_t *mcard[NCH], *m_vpp[NCH], *m_freq[NCH], *m_sub[NCH];
    lv_obj_t *ch_title, *coup[3], *probe[2];
    stepper_t vdiv, pos, tb, lvl;
    lv_obj_t *src[NCH], *slope[2];
    /* the overlays */
    lv_obj_t *ovl, *ta, *kb, *shot_msg, *shot_spin, *shot_img;
    uint32_t shot_seq0, shot_t0;
    bool shot_waiting;
    /* the drag on the grid */
    int32_t drag_y0;
    float drag_off0;
    uint32_t drag_sent_ms;
    bool dragging;
    /* housekeeping */
    uint32_t ticks;
    bool was_connected;
    uint32_t redraws, draw_us;
    uint32_t perf_t0;
    lv_display_t *disp;
    uint64_t refr_t0;
    uint32_t refr_n, refr_us, refr_max;
} U;

/* The screenshot's decoding, in a thread of its own (aos_hal_image_decode is slow by LVGL's measure). */
static struct {
    void *mx;
    int state;                      /* 0 idle, 1 decoding, 2 done */
    bool abandoned;                 /* the overlay went away first: the thread frees what it made */
    char path[160];
    int mw, mh;
    uint16_t *px;
    int w, h;
} D;

static void build_all(void);

/* -------------------------------------------------------------------------- */
/* Numbers                                                                     */
/* -------------------------------------------------------------------------- */

static void commas(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

/* 3.3 -> "3,30 V", 1000 -> "1,000 kHz" (sig = significant digits). */
static void fmt_eng(char *out, size_t n, double v, const char *unit, int sig)
{
    if (!isfinite(v)) { snprintf(out, n, "-- %s", unit); return; }
    static const char *const PFX[] = { "p", "n", MU, "m", "", "k", "M", "G" };
    double a = fabs(v);
    int e3 = a > 0 ? (int)floor(log10(a) / 3.0) : 0;
    if (e3 < -4) e3 = -4;
    if (e3 > 3) e3 = 3;
    char num[32];
    for (int pass = 0; pass < 2; pass++) {
        double m = a / pow(1000.0, e3);
        int intd = m >= 99.95 ? 3 : m >= 9.995 ? 2 : 1;
        int dec = sig - intd < 0 ? 0 : sig - intd;
        snprintf(num, sizeof num, "%.*f", dec, m);
        if (atof(num) >= 1000.0 && e3 < 3) { e3++; continue; }
        break;
    }
    commas(num);
    snprintf(out, n, "%s%s %s%s", v < 0 && atof(num) != 0 ? MINUS : "", num, PFX[e3 + 4], unit);
}

/* A 1-2-5 value: 0.5 -> "500 mV", 2e-6 -> "2 µs". */
static void fmt_125(char *out, size_t n, double v, const char *unit)
{
    static const char *const PFX[] = { "p", "n", MU, "m", "", "k", "M", "G" };
    int e3 = v > 0 ? (int)floor(log10(v * 1.0001) / 3.0) : 0;
    if (e3 < -4) e3 = -4;
    if (e3 > 3) e3 = 3;
    char num[24];
    snprintf(num, sizeof num, "%.3g", v / pow(1000.0, e3));
    commas(num);
    snprintf(out, n, "%s %s%s", num, PFX[e3 + 4], unit);
}

/* The next value of the 1-2-5 sequence, up (dir 1) or down (-1). */
static double step125(double v, int dir)
{
    if (!(v > 0)) return 1;
    double e = floor(log10(v));
    double m = v / pow(10.0, e);
    int k = m < 1.5 ? 0 : m < 3.5 ? 1 : m < 7.5 ? 2 : 3;
    if (k == 3) { k = 0; e += 1; }
    k += dir;
    if (k < 0) { k = 2; e -= 1; }
    if (k > 2) { k = 0; e += 1; }
    static const double M[3] = { 1, 2, 5 };
    return M[k] * pow(10.0, e);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* Style setters that leave an object alone when nothing changes: a set
 * invalidates it even with the same value, and the tick runs ten times a second. */
static void bg_if(lv_obj_t *o, lv_color_t c)
{
    if (o && !lv_color_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0);
}

static void tc_if(lv_obj_t *o, lv_color_t c)
{
    if (o && !lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}

static void border_if(lv_obj_t *o, int32_t w)
{
    if (o && lv_obj_get_style_border_width(o, 0) != w) lv_obj_set_style_border_width(o, w, 0);
}

static void opa_if(lv_obj_t *o, lv_opa_t a)
{
    if (o && lv_obj_get_style_opa(o, 0) != a) lv_obj_set_style_opa(o, a, 0);
}

static void hide_if(lv_obj_t *o, bool hide)
{
    if (o && hide != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static uint64_t now_us(void)
{
    /* only for the costs in the log */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}

/* -------------------------------------------------------------------------- */
/* Small widgets                                                               */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 22, 0);
    return c;
}

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, t, f, c);
    aos_make_decorative(l);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2 < 22 ? h / 2 : 22, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* A square key with a glyph and a word under it. */
static lv_obj_t *key(lv_obj_t *parent, int32_t w, int32_t h, const char *glyph, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = button(parent, w, h, C_BTN, cb, ud);
    lv_obj_align(label(b, glyph, &aos_sym_28, AOS_C_TEXT), LV_ALIGN_TOP_MID, 0, h / 2 - 30);
    lv_obj_align(label(b, text, aos_font_tiny, AOS_C_TEXT), LV_ALIGN_BOTTOM_MID, 0, -(h / 2 - 28));
    return b;
}

/* A segmented choice: a small rounded button, lit when chosen. */
static lv_obj_t *seg(lv_obj_t *parent, int32_t w, int32_t h, const char *text, const lv_font_t *f, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = button(parent, w, h, C_BTN, cb, ud);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_center(label(b, text, f, AOS_C_TEXT));
    return b;
}

static void seg_set(lv_obj_t *b, bool on, lv_color_t lit)
{
    if (!b) return;
    bg_if(b, on ? lit : C_BTN);
    tc_if(lv_obj_get_child(b, 0), on ? lv_color_hex(0x111111) : AOS_C_TEXT);
}

static void repeat_cb(lv_event_t *e)
{
    /* a held -/+ repeats; the release after a hold is not one more step */
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_SHORT_CLICKED && c != LV_EVENT_LONG_PRESSED_REPEAT) return;
    lv_event_cb_t cb = (lv_event_cb_t)lv_event_get_user_data(e);
    cb(e);
}

/* [-]  caption / value  [+]; tapping the value does 'mid_cb' (may be NULL). */
static void stepper(stepper_t *s, lv_obj_t *parent, int32_t w, const char *cap, lv_event_cb_t step_cb, lv_event_cb_t mid_cb)
{
    const int32_t h = 64, bw = 64;
    s->box = box(parent, w, h);
    s->minus = button(s->box, bw, h, C_BTN, NULL, NULL);
    lv_obj_center(label(s->minus, AOS_SYM_MINUS, &aos_sym_28, AOS_C_TEXT));
    lv_obj_add_event_cb(s->minus, repeat_cb, LV_EVENT_ALL, (void *)step_cb);
    lv_obj_set_user_data(s->minus, (void *)(intptr_t)-1);
    s->plus = button(s->box, bw, h, C_BTN, NULL, NULL);
    lv_obj_set_x(s->plus, w - bw);
    lv_obj_center(label(s->plus, AOS_SYM_PLUS, &aos_sym_28, AOS_C_TEXT));
    lv_obj_add_event_cb(s->plus, repeat_cb, LV_EVENT_ALL, (void *)step_cb);
    lv_obj_set_user_data(s->plus, (void *)(intptr_t)1);
    lv_obj_t *mid = box(s->box, w - 2 * bw - 8, h);
    lv_obj_set_x(mid, bw + 4);
    if (mid_cb) {
        lv_obj_add_flag(mid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mid, mid_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_radius(mid, 14, 0);
        lv_obj_set_style_bg_color(mid, C_BTN, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    s->cap = label(mid, cap, aos_font_tiny, AOS_C_DIM);
    lv_obj_align(s->cap, LV_ALIGN_TOP_MID, 0, 2);
    s->val = label(mid, "", aos_font_body, AOS_C_TEXT);
    lv_obj_align(s->val, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static int step_dir(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                    */
/* -------------------------------------------------------------------------- */

static void cmdf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void cmdf(const char *fmt, ...)
{
    char b[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (!aos_scope_cmd(b)) aos_hal_log("scope", "not sent: %s", b);
}

static void run_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    cmdf(!strcmp(st.trig_status, "STOP") ? ":RUN" : ":STOP");
}

static void single_cb(lv_event_t *e) { cmdf(":SING"); }
static void auto_cb(lv_event_t *e) { cmdf(":AUT"); aos_ui_toast(_("Autoajuste: el osciloscopio busca las señales…"), 2500); }

static void chip_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    aos_scope_status_t st;
    aos_scope_status(&st);
    if (!st.ch[c].on) { cmdf(":CHAN%d:DISP 1", c + 1); P.sel = c; }
    else if (P.sel != c) P.sel = c;
    else {
        cmdf(":CHAN%d:DISP 0", c + 1);
        for (int k = 1; k < NCH; k++) if (st.ch[(c + k) % NCH].on) { P.sel = (c + k) % NCH; break; }
    }
    U.drawn_key = 0;
}

static void coup_cb(lv_event_t *e)
{
    static const char *const K[3] = { "DC", "AC", "GND" };
    cmdf(":CHAN%d:COUP %s", P.sel + 1, K[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void probe_cb(lv_event_t *e) { cmdf(":CHAN%d:PROB %d", P.sel + 1, (int)(intptr_t)lv_event_get_user_data(e)); }

static void vdiv_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    const aos_scope_chan_t *c = &st.ch[P.sel];
    double p = c->probe > 0 ? c->probe : 1;
    double v = step125(c->scale, step_dir(e));
    if (v < 1e-3 * p * 0.999 || v > 10 * p * 1.001) return;
    cmdf(":CHAN%d:SCAL %.4e", P.sel + 1, v);
}

static void pos_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    const aos_scope_chan_t *c = &st.ch[P.sel];
    double v = c->offset + step_dir(e) * c->scale / 2;          /* half a division a tap */
    v = round(v / (c->scale / 50)) * (c->scale / 50);
    cmdf(":CHAN%d:OFFS %.4e", P.sel + 1, v);
}

static void pos_zero_cb(lv_event_t *e) { cmdf(":CHAN%d:OFFS 0", P.sel + 1); }

static void tb_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    double v = step125(st.timebase, step_dir(e));
    if (v < 5e-9 * 0.999 || v > 50 * 1.001) return;
    cmdf(":TIM:MAIN:SCAL %.4e", v);
}

static void lvl_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    int s = st.trig_source >= 1 && st.trig_source <= NCH ? st.trig_source - 1 : 0;
    double step = st.ch[s].scale / 5;                               /* a fifth of the source's division */
    double v = round((st.trig_level + step_dir(e) * step) / (step / 20)) * (step / 20);
    cmdf(":TRIG:EDGE:LEV %.4e", v);
}

/* Tapping the level: the middle of the source's signal, the scope's "50%". */
static void lvl_mid_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    int s = st.trig_source >= 1 && st.trig_source <= NCH ? st.trig_source : 1;
    float hi, lo;
    if (aos_scope_measure(s, AOS_SCOPE_M_VMAX, &hi, NULL) && aos_scope_measure(s, AOS_SCOPE_M_VMIN, &lo, NULL))
        cmdf(":TRIG:EDGE:LEV %.4e", (hi + lo) / 2);
    else aos_ui_toast(_("Todavía no hay medidas de ese canal"), 1800);
}

static void src_cb(lv_event_t *e) { cmdf(":TRIG:EDGE:SOUR CHAN%d", (int)(intptr_t)lv_event_get_user_data(e) + 1); }
static void slope_cb(lv_event_t *e) { cmdf(":TRIG:EDGE:SLOP %s", lv_event_get_user_data(e) ? "NEG" : "POS"); }

/* Dragging on the grid moves the chosen channel. */
static void drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || !U.dy) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    aos_scope_status_t st;
    if (code == LV_EVENT_PRESSED) {
        aos_scope_status(&st);
        U.drag_y0 = p.y;
        U.drag_off0 = st.ch[P.sel].offset;
        U.dragging = st.connected && st.ch[P.sel].on;
        return;
    }
    if (!U.dragging || (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)) return;
    aos_scope_status(&st);
    const aos_scope_chan_t *c = &st.ch[P.sel];
    double v = U.drag_off0 + (double)(U.drag_y0 - p.y) / U.dy * c->scale;
    v = round(v / (c->scale / 25)) * (c->scale / 25);
    uint32_t t = now_ms();
    bool last = code == LV_EVENT_RELEASED;
    if (fabs(v - c->offset) > c->scale / 100 && (last || t - U.drag_sent_ms >= 90)) {
        U.drag_sent_ms = t;
        cmdf(":CHAN%d:OFFS %.4e", P.sel + 1, v);
    }
    if (last) U.dragging = false;
}

/* -------------------------------------------------------------------------- */
/* The grid                                                                    */
/* -------------------------------------------------------------------------- */

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
}

static uint16_t half(uint16_t a, uint16_t b) { return (uint16_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1)); }

static void vspan(int x, int y0, int y1, uint16_t col)
{
    if (x < 0 || x >= U.tw) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < 0 || y0 >= U.th) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= U.th) y1 = U.th - 1;
    uint16_t *p = U.px + (size_t)y0 * U.stride + x;
    for (int y = y0; y <= y1; y++, p += U.stride) *p = col;
}

static void draw_grid(void)
{
    const uint16_t bg = rgb565(RGB_BG), g = rgb565(RGB_GRID), a = rgb565(RGB_AXIS);
    uint16_t *row = U.px;
    for (int x = 0; x < U.tw; x++) row[x] = bg;
    for (int y = 1; y < U.th; y++) memcpy(U.px + (size_t)y * U.stride, row, (size_t)U.tw * 2);
    /* the divisions, dotted */
    for (int k = 1; k < 12; k++) {
        int x = k * U.dx;
        for (int y = 0; y < U.th; y += 4) U.px[(size_t)y * U.stride + x] = k == 6 ? a : g;
    }
    for (int j = 1; j < 8; j++) {
        uint16_t *p = U.px + (size_t)(j * U.dy) * U.stride;
        for (int x = 0; x < U.tw; x += 4) p[x] = j == 4 ? a : g;
    }
    /* the ticks along the centre lines, five a division */
    int cx = 6 * U.dx, cy = 4 * U.dy;
    for (int i = 0; i <= 60; i++) {
        int x = i * U.tw / 60;
        vspan(x < U.tw ? x : U.tw - 1, cy - 3, cy + 3, a);
    }
    for (int i = 0; i <= 40; i++) {
        int y = i * U.th / 40;
        if (y >= U.th) y = U.th - 1;
        uint16_t *p = U.px + (size_t)y * U.stride;
        for (int x = cx - 3; x <= cx + 3; x++) p[x] = a;
    }
    /* the frame */
    for (int x = 0; x < U.tw; x++) { U.px[x] = a; U.px[(size_t)(U.th - 1) * U.stride + x] = a; }
    vspan(0, 0, U.th - 1, a);
    vspan(U.tw - 1, 0, U.th - 1, a);
}

static int volt_y(float v, const aos_scope_chan_t *c)
{
    float div = (v + c->offset) / c->scale;
    float y = (float)U.th / 2 - div * (float)U.dy;
    if (y < -1000) y = -1000;
    if (y > 3000) y = 3000;
    return (int)lroundf(y);
}

static void draw_wave(int c, const aos_scope_chan_t *ch)
{
    int n = U.wave_n[c];
    if (n < 2 || !(ch->scale > 0)) return;
    const float *v = U.wave + c * AOS_SCOPE_POINTS;
    const uint16_t col = rgb565(CH_RGB[c]);
    int colx = -1, lo = 0, hi = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        int x = (int)((int64_t)i * (U.tw - 1) / (n - 1));
        int y = volt_y(v[i], ch);
        if (x != colx) {
            if (colx >= 0) {
                vspan(colx, lo, hi + 1, col);
                vspan(colx + 1, lo, hi + 1, col);
                for (int xs = colx + 1; xs < x; xs++) {        /* fewer points than pixels */
                    int yi = prev + (y - prev) * (xs - colx) / (x - colx);
                    vspan(xs, yi, yi + 1, col);
                }
            }
            lo = hi = y;
            if (colx >= 0) { if (prev < lo) lo = prev; if (prev > hi) hi = prev; }
            colx = x;
        } else {
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        prev = y;
    }
    if (colx >= 0) vspan(colx, lo, hi + 1, col);
}

static uint32_t draw_key(const aos_scope_status_t *st)
{
    /* what moves the picture, besides a new set of traces */
    uint32_t k = 2166136261u;
    const uint8_t *p;
#define MIX(x) do { p = (const uint8_t *)&(x); for (size_t _i = 0; _i < sizeof(x); _i++) k = (k ^ p[_i]) * 16777619u; } while (0)
    for (int c = 0; c < NCH; c++) { MIX(st->ch[c].on); MIX(st->ch[c].scale); MIX(st->ch[c].offset); }
    MIX(st->trig_level);
    MIX(st->trig_source);
    MIX(st->wave_seq);
    MIX(P.sel);
    bool dash = now_ms() - U.trig_changed_ms < 2000;
    MIX(dash);
#undef MIX
    return k | 1;
}

static void place_markers(const aos_scope_status_t *st)
{
    for (int c = 0; c < NCH; c++) {
        lv_obj_t *g = U.gnd[c];
        if (!g) continue;
        bool hide = !st->connected || !st->ch[c].on || !(st->ch[c].scale > 0);
        hide_if(g, hide);
        if (hide) continue;
        int y = volt_y(0, &st->ch[c]) - 13;
        if (y < 0) y = 0;
        if (y > U.th - 26) y = U.th - 26;
        if (lv_obj_get_y(g) != y) lv_obj_set_y(g, y);
        border_if(g, c == P.sel ? 2 : 0);
    }
    int s = st->trig_source - 1;
    if (st->connected && s >= 0 && s < NCH && st->ch[s].on && st->ch[s].scale > 0) {
        hide_if(U.trig_tag, false);
        int y = volt_y(st->trig_level, &st->ch[s]) - 13;
        if (y < 0) y = 0;
        if (y > U.th - 26) y = U.th - 26;
        if (lv_obj_get_y(U.trig_tag) != y) lv_obj_set_y(U.trig_tag, y);
    } else {
        hide_if(U.trig_tag, true);
    }
}

static void redraw(const aos_scope_status_t *st)
{
    if (!U.px) return;
    uint64_t t0 = now_us();
    draw_grid();
    if (st->connected) {
        /* the chosen channel on top */
        for (int k = 1; k <= NCH; k++) {
            int c = (P.sel + k) % NCH;
            if (st->ch[c].on) draw_wave(c, &st->ch[c]);
        }
        int s = st->trig_source - 1;
        if (s >= 0 && s < NCH && st->ch[s].on && now_ms() - U.trig_changed_ms < 2000) {
            int y = volt_y(st->trig_level, &st->ch[s]);
            if (y >= 0 && y < U.th) {
                uint16_t col = half(rgb565(RGB_TRIG), rgb565(RGB_BG)), *p = U.px + (size_t)y * U.stride;
                for (int x = 0; x < U.tw; x++) if ((x & 7) < 4) p[x] = col;
            }
        }
    }
    /* the trigger's place in time, the centre: a small triangle on top */
    const uint16_t tc = rgb565(RGB_TRIG);
    for (int r = 0; r < 9; r++) {
        uint16_t *p = U.px + (size_t)(r + 1) * U.stride + 6 * U.dx;
        for (int x = -(8 - r); x <= 8 - r; x++) p[x] = tc;
    }
    lv_obj_invalidate(U.canvas);
    place_markers(st);
    U.redraws++;
    U.draw_us += (uint32_t)(now_us() - t0);
}

static void canvas_delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

/* -------------------------------------------------------------------------- */
/* Overlays: the host, the scope's screenshot, the connection card             */
/* -------------------------------------------------------------------------- */

static void ovl_close(void)
{
    if (U.ovl) lv_obj_delete(U.ovl);        /* the screenshot's pixels go with its image (delete event) */
    U.ovl = U.ta = U.kb = U.shot_msg = U.shot_spin = U.shot_img = NULL;
    U.shot_waiting = false;
    if (D.mx) {
        aos_hal_mutex_lock(D.mx);
        if (D.state == 1) D.abandoned = true;
        else if (D.state == 2) { aos_hal_image_free(D.px); D.px = NULL; D.state = 0; }
        aos_hal_mutex_unlock(D.mx);
    }
}

static lv_obj_t *ovl_open(uint32_t bg, lv_opa_t opa)
{
    ovl_close();
    U.ovl = box(U.root, U.w, U.h);
    lv_obj_set_style_bg_color(U.ovl, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(U.ovl, opa, 0);
    lv_obj_set_style_radius(U.ovl, 22, 0);
    lv_obj_add_flag(U.ovl, LV_OBJ_FLAG_CLICKABLE);     /* nothing under it takes a touch */
    return U.ovl;
}

static void host_done(const char *h)
{
    char host[80];
    snprintf(host, sizeof host, "%s", h);
    char *s = host;
    while (*s == ' ') s++;
    size_t l = strlen(s);
    while (l && s[l - 1] == ' ') s[--l] = 0;
    int port = 0;
    char *c = strrchr(s, ':');
    if (c) { *c = 0; port = atoi(c + 1); }
    if (!s[0]) return;
    aos_scope_connect(s, port);
    P.conn_open = false;
}

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[80];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(U.ta));
    ovl_close();
    if (c == LV_EVENT_READY) host_done(v);
}

static void host_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    char v[80] = "";
    int32_t port = 5555;
    aos_hal_pref_get_i32("scope_port", &port);
    if (st.host[0]) snprintf(v, sizeof v, port != 5555 ? "%s:%d" : "%s", st.host, (int)port);
    lv_obj_t *o = ovl_open(0x121216, LV_OPA_COVER);
    lv_obj_t *t = label(o, _("Dirección del osciloscopio"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 24);
    lv_obj_t *hint = label(o, _("La IP que muestra el Rigol en Utility → IO Setting → LAN Conf. El puerto es 5555 si no ponés otro (192.168.1.50:5555)."),
                           aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(hint, U.w - 48);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 24, 80);
    U.ta = lv_textarea_create(o);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_text(U.ta, v);
    lv_textarea_set_placeholder_text(U.ta, "192.168.1.50");
    lv_obj_set_size(U.ta, U.w - 48, 88);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, 24, 150);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    U.kb = lv_keyboard_create(o);
    lv_obj_set_size(U.kb, U.w, U.land ? U.h / 2 : U.h * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the screenshot ---- */

static void decode_thread(void *arg)
{
    (void)arg;
    char path[160];
    int mw, mh;
    aos_hal_mutex_lock(D.mx);
    snprintf(path, sizeof path, "%s", D.path);
    mw = D.mw;
    mh = D.mh;
    aos_hal_mutex_unlock(D.mx);
    int w = 0, h = 0;
    uint16_t *px = aos_hal_image_decode(path, 0, 0, mw, mh, false, &w, &h, NULL, NULL);
    aos_hal_mutex_lock(D.mx);
    if (D.abandoned) {
        if (px) aos_hal_image_free(px);
        D.abandoned = false;
        D.state = 0;
    } else {
        D.px = px;
        D.w = w;
        D.h = h;
        D.state = 2;
    }
    aos_hal_mutex_unlock(D.mx);
}

static void shot_img_delete_cb(lv_event_t *e)
{
    lv_image_dsc_t *d = lv_event_get_user_data(e);
    if (d) { aos_hal_image_free((void *)d->data); free(d); }
}

static void shot_close_cb(lv_event_t *e) { ovl_close(); }

static void shot_cb(lv_event_t *e)
{
    uint32_t seq;
    aos_scope_screenshot_file(&seq);
    if (!aos_scope_screenshot()) { aos_ui_toast(_("No hay osciloscopio conectado"), 1800); return; }
    lv_obj_t *o = ovl_open(0x000000, LV_OPA_COVER);
    lv_obj_add_event_cb(o, shot_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(o, _("Pantalla del osciloscopio"), aos_font_body, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 16, 18);
    lv_obj_t *x = button(o, 64, 64, C_BTN, shot_close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_center(label(x, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT));
    U.shot_spin = lv_spinner_create(o);
    lv_obj_set_size(U.shot_spin, 72, 72);
    lv_obj_set_style_arc_width(U.shot_spin, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.shot_spin, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(U.shot_spin, AOS_C_ORANGE, LV_PART_INDICATOR);
    lv_obj_align(U.shot_spin, LV_ALIGN_CENTER, 0, -30);
    aos_make_decorative(U.shot_spin);
    U.shot_msg = label(o, _("Pidiendo la pantalla al osciloscopio…"), aos_font_small, AOS_C_DIM);
    lv_obj_align(U.shot_msg, LV_ALIGN_CENTER, 0, 50);
    U.shot_seq0 = seq;
    U.shot_t0 = now_ms();
    U.shot_waiting = true;
}

static void shot_tick(void)
{
    if (!U.ovl || !U.shot_msg) return;
    uint32_t seq;
    const char *path = aos_scope_screenshot_file(&seq);
    if (U.shot_waiting && seq != U.shot_seq0) {
        if (!D.mx) D.mx = aos_hal_mutex_create();
        aos_hal_mutex_lock(D.mx);
        bool idle = D.state == 0;
        if (idle) {
            snprintf(D.path, sizeof D.path, "%s", path);
            D.mw = U.w - 16;
            D.mh = U.h - 96;
            D.state = 1;
            D.abandoned = false;
        }
        aos_hal_mutex_unlock(D.mx);
        if (!idle) return;              /* the last one is still being decoded for an overlay that left */
        U.shot_waiting = false;
        set_text(U.shot_msg, _("Abriendo la imagen…"));
        if (!aos_hal_thread_start("scope_png", decode_thread, NULL, 8192, 3)) {
            aos_hal_mutex_lock(D.mx);
            D.state = 0;
            aos_hal_mutex_unlock(D.mx);
            set_text(U.shot_msg, _("Sin memoria para abrir la imagen"));
            lv_obj_add_flag(U.shot_spin, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (U.shot_waiting) {
        if (!aos_scope_screenshot_busy() && now_ms() - U.shot_t0 > 500) {
            U.shot_waiting = false;
            set_text(U.shot_msg, _("El osciloscopio no mandó su pantalla"));
            lv_obj_add_flag(U.shot_spin, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!D.mx || U.shot_img) return;
    aos_hal_mutex_lock(D.mx);
    bool done = D.state == 2;
    uint16_t *px = D.px;
    int w = D.w, h = D.h;
    if (done) { D.px = NULL; D.state = 0; }
    aos_hal_mutex_unlock(D.mx);
    if (!done) return;
    lv_obj_add_flag(U.shot_spin, LV_OBJ_FLAG_HIDDEN);
    if (!px) { set_text(U.shot_msg, _("No se pudo leer la imagen")); return; }
    lv_image_dsc_t *d = calloc(1, sizeof *d);
    if (!d) { aos_hal_image_free(px); return; }
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 2u;
    d->data_size = (uint32_t)(w * h * 2);
    d->data = (const uint8_t *)px;
    U.shot_img = lv_image_create(U.ovl);
    lv_image_set_src(U.shot_img, d);
    lv_obj_add_event_cb(U.shot_img, shot_img_delete_cb, LV_EVENT_DELETE, d);
    lv_obj_align(U.shot_img, LV_ALIGN_CENTER, 0, 36);
    aos_make_decorative(U.shot_img);
    char t[64];
    snprintf(t, sizeof t, "%d × %d · %s", w, h, _("tocá para cerrar"));
    set_text(U.shot_msg, t);
    lv_obj_align(U.shot_msg, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* ---- the connection card ---- */

static void conn_go_cb(lv_event_t *e)
{
    aos_scope_status_t st;
    aos_scope_status(&st);
    if (st.connected || st.connecting) { aos_scope_disconnect(); P.conn_open = false; }
    else if (st.host[0]) {
        int32_t port = 5555;
        aos_hal_pref_get_i32("scope_port", &port);
        aos_scope_connect(st.host, port);
    } else host_cb(e);
}

static void conn_close_cb(lv_event_t *e) { P.conn_open = false; }
static void link_cb(lv_event_t *e) { P.conn_open = !P.conn_open; }

static void build_conn(lv_obj_t *parent)
{
    int32_t cw = U.tw - 48 < 560 ? U.tw - 48 : 560;
    U.conn = card(parent, cw, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(U.conn, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(U.conn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(U.conn, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(U.conn, 1, 0);
    lv_obj_set_style_pad_all(U.conn, 20, 0);
    lv_obj_set_flex_flow(U.conn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.conn, 12, 0);
    lv_obj_add_flag(U.conn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(U.conn);
    lv_obj_t *top = box(U.conn, lv_pct(100), 44);
    lv_obj_align(label(top, AOS_SYM_SINE_WAVE, &aos_sym_44, ch_color(0)), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(label(top, _("Osciloscopio Rigol"), aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 58, 0);
    U.conn_close = button(top, 44, 44, C_BTN, conn_close_cb, NULL);
    lv_obj_align(U.conn_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_center(label(U.conn_close, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_TEXT));
    U.conn_status = label(U.conn, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.conn_status, lv_pct(100));
    lv_label_set_long_mode(U.conn_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *row = box(U.conn, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *hb = button(row, cw - 40 - 12 - 190, 64, C_BTN, host_cb, NULL);
    lv_obj_set_style_radius(hb, 16, 0);
    U.conn_host = label(hb, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(U.conn_host, cw - 40 - 12 - 190 - 36);
    lv_label_set_long_mode(U.conn_host, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(U.conn_host, LV_ALIGN_LEFT_MID, 18, 0);
    U.conn_go = button(row, 190, 64, C_RUN, conn_go_cb, NULL);
    U.conn_go_l = label(U.conn_go, "", aos_font_small, AOS_C_TEXT);
    lv_obj_center(U.conn_go_l);
    lv_obj_t *hint = label(U.conn, _("En el Rigol: Utility → IO Setting → LAN Conf, con la LAN activada. P4OS le habla SCPI por el puerto 5555."),
                           aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
}

static void conn_refresh(const aos_scope_status_t *st)
{
    bool show = !st->connected || P.conn_open;
    hide_if(U.conn, !show);
    if (!show) return;
    char t[160];
    int32_t port = 5555;
    aos_hal_pref_get_i32("scope_port", &port);
    if (st->host[0]) snprintf(t, sizeof t, port != 5555 ? "%s:%d" : "%s", st->host, (int)port);
    else snprintf(t, sizeof t, "%s", _("Tocá para poner la IP"));
    set_text(U.conn_host, t);
    if (st->connected) {
        snprintf(t, sizeof t, "%s", st->idn);
        tc_if(U.conn_status, AOS_C_GREEN);
    } else if (st->connecting) {
        if (st->error[0]) snprintf(t, sizeof t, "%s · %s", st->error, _("reintentando…"));
        else snprintf(t, sizeof t, "%s", _("Conectando…"));
        tc_if(U.conn_status, st->error[0] ? AOS_C_ORANGE : AOS_C_DIM);
    } else if (st->error[0]) {
        snprintf(t, sizeof t, "%s", st->error);
        tc_if(U.conn_status, AOS_C_ORANGE);
    } else {
        snprintf(t, sizeof t, "%s", st->host[0] ? _("Sin conectar.") : _("Poné la IP del osciloscopio para empezar."));
        tc_if(U.conn_status, AOS_C_DIM);
    }
    set_text(U.conn_status, t);
    bool busy = st->connected || st->connecting;
    set_text(U.conn_go_l, st->connected ? _("Desconectar") : st->connecting ? _("Cancelar") : _("Conectar"));
    bg_if(U.conn_go, busy ? C_STOP : C_RUN);
    hide_if(U.conn_close, !st->connected);
}

/* -------------------------------------------------------------------------- */
/* Building                                                                    */
/* -------------------------------------------------------------------------- */

static lv_obj_t *flex(lv_obj_t *parent, int32_t w, int32_t h, lv_flex_flow_t flow, int32_t gap)
{
    lv_obj_t *o = box(parent, w, h);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_gap(o, gap, 0);
    return o;
}

static void build_transport(lv_obj_t *parent, int32_t w)
{
    const int32_t h = 72, gap = 10, rw = w >= 640 ? 200 : 170, kw = w >= 640 ? 96 : 88;
    lv_obj_t *r = box(parent, w, h);
    U.controls[0] = r;
    U.run_btn = button(r, rw, h, C_RUN, run_cb, NULL);
    lv_obj_set_style_radius(U.run_btn, 22, 0);
    lv_obj_set_flex_flow(U.run_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.run_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.run_btn, 10, 0);
    U.run_g = label(U.run_btn, AOS_SYM_PLAY, &aos_sym_28, AOS_C_TEXT);
    U.run_l = label(U.run_btn, _("En marcha"), aos_font_small, AOS_C_TEXT);
    int32_t x = rw + gap;
    lv_obj_set_x(key(r, kw, h, AOS_SYM_STEP_FORWARD, _("Única"), single_cb, NULL), x);
    x += kw + gap;
    lv_obj_set_x(key(r, kw, h, AOS_SYM_AUTO_FIX, _("Auto"), auto_cb, NULL), x);
    x += kw + gap;
    lv_obj_set_x(key(r, kw, h, AOS_SYM_MONITOR_SCREENSHOT, _("Captura"), shot_cb, NULL), x);
    x += kw + gap;
    /* the link: the model and the rate, tap for the connection card */
    int32_t lw = w - x;
    if (lw >= 64) {
        U.link_chip = button(r, lw, h, AOS_C_CARD, link_cb, NULL);
        lv_obj_set_x(U.link_chip, x);
        U.link_g = label(U.link_chip, AOS_SYM_LAN_CONNECT, &aos_sym_28, AOS_C_GREEN);
        if (lw >= 140) {
            lv_obj_align(U.link_g, LV_ALIGN_LEFT_MID, 14, 0);
            U.link_l = label(U.link_chip, "", aos_font_tiny, AOS_C_DIM);
            lv_obj_set_width(U.link_l, lw - 58);
            lv_label_set_long_mode(U.link_l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(U.link_l, LV_ALIGN_LEFT_MID, 50, 0);
        } else {
            lv_obj_center(U.link_g);
        }
    }
}

static void build_trace(lv_obj_t *parent)
{
    U.tracebox = box(parent, U.tw, U.th);
    lv_obj_add_flag(U.tracebox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.tracebox, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(U.tracebox, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.tracebox, drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(U.tracebox, drag_cb, LV_EVENT_RELEASED, NULL);
    U.stride = (int32_t)(lv_draw_buf_width_to_stride((uint32_t)U.tw, LV_COLOR_FORMAT_RGB565) / 2);
    U.px = malloc((size_t)U.stride * (size_t)U.th * 2);
    if (U.px) {
        U.canvas = lv_canvas_create(U.tracebox);
        lv_canvas_set_buffer(U.canvas, U.px, U.tw, U.th, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_style_radius(U.canvas, 0, 0);        /* a radius on a canvas is a mask */
        lv_image_set_antialias(U.canvas, false);
        aos_make_decorative(U.canvas);
        lv_obj_add_event_cb(U.canvas, canvas_delete_cb, LV_EVENT_DELETE, U.px);
    } else {
        lv_obj_set_style_bg_color(U.tracebox, lv_color_hex(RGB_BG), 0);
        lv_obj_set_style_bg_opa(U.tracebox, LV_OPA_COVER, 0);
        aos_hal_log("scope", "no memory for a %dx%d grid", (int)U.tw, (int)U.th);
    }
    /* the state of the trigger, top left, and the time/trigger, top right */
    lv_obj_t *top = flex(U.tracebox, LV_SIZE_CONTENT, 34, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(top, 36, 12);
    U.badge = box(top, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_radius(U.badge, 10, 0);
    lv_obj_set_style_bg_opa(U.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(U.badge, 12, 0);
    U.badge_l = label(U.badge, "", aos_font_caption, lv_color_hex(0x111111));
    lv_obj_center(U.badge_l);
    U.fps_l = label(top, "", aos_font_tiny, AOS_C_DIM);
    U.info_l = label(U.tracebox, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_style_bg_color(U.info_l, lv_color_hex(RGB_BG), 0);
    lv_obj_set_style_bg_opa(U.info_l, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(U.info_l, 6, 0);
    lv_obj_align(U.info_l, LV_ALIGN_TOP_RIGHT, -12, 16);
    for (int c = 0; c < NCH; c++) {
        lv_obj_t *g = box(U.tracebox, 28, 26);
        lv_obj_set_style_bg_color(g, ch_color(c), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g, 6, 0);
        lv_obj_set_style_border_color(g, lv_color_white(), 0);
        char n[4];
        snprintf(n, sizeof n, "%d", c + 1);
        lv_obj_center(label(g, n, aos_font_caption, lv_color_hex(0x111111)));
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
        U.gnd[c] = g;
    }
    U.trig_tag = box(U.tracebox, 28, 26);
    lv_obj_set_style_bg_color(U.trig_tag, lv_color_hex(RGB_TRIG), 0);
    lv_obj_set_style_bg_opa(U.trig_tag, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.trig_tag, 6, 0);
    lv_obj_set_x(U.trig_tag, U.tw - 28);
    lv_obj_center(label(U.trig_tag, "T", aos_font_caption, lv_color_hex(0x111111)));
    lv_obj_add_flag(U.trig_tag, LV_OBJ_FLAG_HIDDEN);
    build_conn(U.tracebox);
}

static void build_meas(lv_obj_t *parent, int32_t w)
{
    const int32_t gap = 10, cw = (w - gap) / 2, ch = 66;
    lv_obj_t *g = flex(parent, w, 2 * ch + gap, LV_FLEX_FLOW_ROW_WRAP, gap);
    for (int c = 0; c < NCH; c++) {
        lv_obj_t *k = card(g, cw, ch);
        lv_obj_set_style_radius(k, 16, 0);
        lv_obj_t *bar = box(k, 6, ch - 20);
        lv_obj_set_style_bg_color(bar, ch_color(c), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_pos(bar, 10, 10);
        const lv_font_t *f = cw >= 320 ? aos_font_body : aos_font_small;
        int32_t y = cw >= 320 ? 2 : 6;
        U.m_vpp[c] = label(k, "", f, AOS_C_TEXT);
        lv_obj_set_pos(U.m_vpp[c], 26, y);
        U.m_freq[c] = label(k, "", f, AOS_C_TEXT);
        lv_obj_align(U.m_freq[c], LV_ALIGN_TOP_RIGHT, -14, y);
        U.m_sub[c] = label(k, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(U.m_sub[c], cw - 40);
        lv_label_set_long_mode(U.m_sub[c], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(U.m_sub[c], 26, 40);
        U.mcard[c] = k;
    }
}

static void build_chips(lv_obj_t *parent, int32_t w, int cols)
{
    const int32_t gap = 10, ch = 64;
    int rows = NCH / cols;
    int32_t cw = (w - (cols - 1) * gap) / cols;
    lv_obj_t *g = flex(parent, w, rows * ch + (rows - 1) * gap, LV_FLEX_FLOW_ROW_WRAP, gap);
    U.controls[1] = g;
    for (int c = 0; c < NCH; c++) {
        lv_obj_t *k = button(g, cw, ch, AOS_C_CARD, chip_cb, (void *)(intptr_t)c);
        lv_obj_set_style_radius(k, 16, 0);
        lv_obj_set_style_border_color(k, ch_color(c), 0);
        U.chip_name[c] = label(k, "", aos_font_caption, ch_color(c));
        lv_obj_set_pos(U.chip_name[c], 16, 6);
        U.chip_val[c] = label(k, "", aos_font_small, AOS_C_TEXT);
        lv_obj_set_pos(U.chip_val[c], 16, 30);
        U.chip[c] = k;
    }
}

/* The chosen channel's card, then the time and trigger card. 'stack' puts
 * the two steppers of a card one over the other. */
static void build_cards(lv_obj_t *parent, int32_t w, bool stack)
{
    const int32_t pad = 14, gap = 10;
    const int32_t inner = w - 2 * pad;
    const int32_t sw = stack ? inner : (inner - gap) / 2;
    const int32_t sh = 44;
    lv_obj_t *col = flex(parent, w, LV_SIZE_CONTENT, LV_FLEX_FLOW_COLUMN, gap);
    U.controls[2] = col;

    /* the channel */
    lv_obj_t *k = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(k, pad, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(k, gap, 0);
    lv_obj_t *head = box(k, inner, sh);
    U.ch_title = label(head, "", stack ? aos_font_small : aos_font_body, AOS_C_TEXT);
    lv_obj_align(U.ch_title, LV_ALIGN_LEFT_MID, 4, 0);
    static const char *const CP[3] = { "DC", "AC", "GND" };
    int32_t segw = (inner - (stack ? 56 : 80) - 16 - 4 * 6) / 5;
    if (segw > 62) segw = 62;
    int32_t x = inner - 2 * segw - 6 - 16 - 3 * segw - 2 * 6;
    for (int i = 0; i < 3; i++) {
        U.coup[i] = seg(head, segw, sh, CP[i], aos_font_caption, coup_cb, (void *)(intptr_t)i);
        lv_obj_set_x(U.coup[i], x);
        x += segw + 6;
    }
    x = inner - 2 * segw - 6;
    U.probe[0] = seg(head, segw, sh, "×1", aos_font_caption, probe_cb, (void *)(intptr_t)1);
    lv_obj_set_x(U.probe[0], x);
    U.probe[1] = seg(head, segw, sh, "×10", aos_font_caption, probe_cb, (void *)(intptr_t)10);
    lv_obj_set_x(U.probe[1], x + segw + 6);
    stepper(&U.vdiv, k, sw, _("V/div"), vdiv_cb, NULL);
    stepper(&U.pos, k, sw, _("Posición · tocá para 0"), pos_cb, pos_zero_cb);

    /* time and trigger */
    k = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(k, pad, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(k, gap, 0);
    head = box(k, inner, sh);
    lv_obj_align(label(head, _("Disparo"), stack ? aos_font_caption : aos_font_body, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, 0);
    int32_t sgw = (inner - (stack ? 76 : 120) - 16 - 5 * 6) / 6;
    if (sgw > 56) sgw = 56;
    x = inner - 2 * sgw - 6 - 16 - NCH * sgw - (NCH - 1) * 6;
    for (int c = 0; c < NCH; c++) {
        char n[4];
        snprintf(n, sizeof n, "%d", c + 1);
        U.src[c] = seg(head, sgw, sh, n, aos_font_small, src_cb, (void *)(intptr_t)c);
        lv_obj_set_x(U.src[c], x);
        x += sgw + 6;
    }
    x = inner - 2 * sgw - 6;
    U.slope[0] = seg(head, sgw, sh, AOS_SYM_TRENDING_UP, &aos_sym_28, slope_cb, (void *)(intptr_t)0);
    lv_obj_set_x(U.slope[0], x);
    U.slope[1] = seg(head, sgw, sh, AOS_SYM_TRENDING_DOWN, &aos_sym_28, slope_cb, (void *)(intptr_t)1);
    lv_obj_set_x(U.slope[1], x + sgw + 6);
    stepper(&U.tb, k, sw, _("Base de tiempo"), tb_cb, NULL);
    stepper(&U.lvl, k, sw, _("Nivel · tocá para el 50 %"), lvl_cb, lvl_mid_cb);
}

static void build_all(void)
{
    const int32_t gap = 10;
    lv_obj_t *root = U.root;
    if (!U.land) {
        /* upright: transport, grid, measurements, chips, cards - the grid takes what is left */
        const int32_t rest = 72 + 140 + 64 + 2 * (14 + 44 + 10 + 64 + 14) + 6 * gap;
        U.dx = U.w / 12;
        int32_t dy = (U.h - rest) / 8;
        int32_t ideal = U.dx * 12 * 10 / 16 / 8;          /* 16:10 */
        if (dy > ideal) dy = ideal;
        if (dy < 30) dy = 30;
        U.dy = dy;
        U.tw = 12 * U.dx;
        U.th = 8 * U.dy;
        lv_obj_t *col = flex(root, U.w, U.h, LV_FLEX_FLOW_COLUMN, gap);
        lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);       /* only if the room was short */
        lv_obj_set_scroll_dir(col, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        build_transport(col, U.w);
        build_trace(col);
        build_meas(col, U.w);
        build_chips(col, U.w, 4);
        build_cards(col, U.w, false);
    } else {
        /* lying down: transport, grid and measurements on the left, the rest on the right */
        const int32_t rw_min = 390;
        int32_t d = (U.h - 72 - 140 - 2 * gap) / 8;
        int32_t dmax = (U.w - rw_min - 16) / 12;
        if (d > dmax) d = dmax;
        if (d < 30) d = 30;
        U.dx = U.dy = d;
        U.tw = 12 * d;
        U.th = 8 * d;
        lv_obj_t *left = flex(root, U.tw, U.h, LV_FLEX_FLOW_COLUMN, gap);
        lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        build_transport(left, U.tw);
        build_trace(left);
        build_meas(left, U.tw);
        int32_t rw = U.w - U.tw - 16;
        lv_obj_t *right = flex(root, rw, U.h, LV_FLEX_FLOW_COLUMN, gap);
        lv_obj_set_x(right, U.tw + 16);
        lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(right, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
        build_chips(right, rw, 2);
        build_cards(right, rw, true);
    }
}

/* -------------------------------------------------------------------------- */
/* Refreshing                                                                  */
/* -------------------------------------------------------------------------- */

static const char *status_word(const char *s, lv_color_t *bg)
{
    if (!strcmp(s, "TD")) { *bg = AOS_C_GREEN; return _("Disparado"); }
    if (!strcmp(s, "WAIT")) { *bg = AOS_C_ORANGE; return _("Esperando"); }
    if (!strcmp(s, "AUTO")) { *bg = AOS_C_TEAL; return _("Auto"); }
    if (!strcmp(s, "RUN")) { *bg = AOS_C_GREEN; return _("En marcha"); }
    if (!strcmp(s, "STOP")) { *bg = AOS_C_RED; return _("Detenido"); }
    *bg = AOS_C_DIM;
    return s[0] ? s : "--";
}

static void refresh_controls(const aos_scope_status_t *st)
{
    char t[64], v[32];
    bool on = st->connected;
    if (on != U.was_connected) {
        U.was_connected = on;
        for (int i = 0; i < 3; i++) {
            if (!U.controls[i]) continue;
            opa_if(U.controls[i], on ? LV_OPA_COVER : LV_OPA_40);
            if (on) lv_obj_remove_state(U.controls[i], LV_STATE_DISABLED);
            else lv_obj_add_state(U.controls[i], LV_STATE_DISABLED);
        }
        U.drawn_key = 0;
    }
    /* transport */
    bool stopped = !strcmp(st->trig_status, "STOP");
    bg_if(U.run_btn, stopped ? C_STOP : C_RUN);
    set_text(U.run_g, stopped ? AOS_SYM_STOP : AOS_SYM_PLAY);
    set_text(U.run_l, stopped ? _("Detenido") : _("En marcha"));
    if (U.link_l) {
        const char *model = st->idn;
        char m[40] = "";
        const char *c1 = strchr(model, ',');
        if (c1) { snprintf(m, sizeof m, "%.*s", (int)strcspn(c1 + 1, ","), c1 + 1); }
        if (on) snprintf(t, sizeof t, "%s\n%.0f fps", m[0] ? m : "Rigol", (double)st->fps);
        else snprintf(t, sizeof t, "%s", st->connecting ? _("conectando…") : _("sin conectar"));
        set_text(U.link_l, t);
    }
    if (U.link_g) tc_if(U.link_g, on ? AOS_C_GREEN : st->connecting ? AOS_C_ORANGE : AOS_C_DIM);
    if (U.link_g) set_text(U.link_g, on || st->connecting ? AOS_SYM_LAN_CONNECT : AOS_SYM_LAN_DISCONNECT);

    /* the labels on the grid */
    lv_color_t bg;
    const char *w = status_word(on ? st->trig_status : "", &bg);
    set_text(U.badge_l, on ? w : _("Sin osciloscopio"));
    bg_if(U.badge, on ? bg : AOS_C_CARD2);
    tc_if(U.badge_l, on ? lv_color_hex(0x111111) : AOS_C_DIM);
    if (on) snprintf(t, sizeof t, "%.1f fps", (double)st->fps);
    else t[0] = 0;
    commas(t);
    set_text(U.fps_l, t);
    if (on) {
        char tb[24], lv[24];
        fmt_125(tb, sizeof tb, st->timebase, "s");
        fmt_eng(lv, sizeof lv, st->trig_level, "V", 3);
        const char *sl = !strcmp(st->trig_slope, "NEG") ? "↓" : !strcmp(st->trig_slope, "RFAL") ? "↑↓" : "↑";
        snprintf(t, sizeof t, "H %s   T%d %s %s", tb, st->trig_source, sl, lv);
    } else t[0] = 0;
    set_text(U.info_l, t);
    hide_if(U.info_l, !t[0]);

    /* chips */
    for (int c = 0; c < NCH; c++) {
        const aos_scope_chan_t *ch = &st->ch[c];
        snprintf(t, sizeof t, "CH%d", c + 1);
        set_text(U.chip_name[c], t);
        if (ch->on) fmt_125(v, sizeof v, ch->scale, "V");
        else snprintf(v, sizeof v, "%s", _("apagado"));
        set_text(U.chip_val[c], v);
        tc_if(U.chip_name[c], ch->on ? ch_color(c) : AOS_C_DIM);
        tc_if(U.chip_val[c], ch->on ? AOS_C_TEXT : AOS_C_DIM);
        border_if(U.chip[c], c == P.sel ? 3 : 0);
        bg_if(U.chip[c], c == P.sel ? lv_color_mix(ch_color(c), AOS_C_CARD, 40) : AOS_C_CARD);
    }
    /* the chosen channel */
    const aos_scope_chan_t *ch = &st->ch[P.sel];
    snprintf(t, sizeof t, "CH%d", P.sel + 1);
    set_text(U.ch_title, t);
    tc_if(U.ch_title, ch_color(P.sel));
    static const char *const CP[3] = { "DC", "AC", "GND" };
    for (int i = 0; i < 3; i++) seg_set(U.coup[i], !strcmp(ch->coupling, CP[i]), ch_color(P.sel));
    seg_set(U.probe[0], fabsf(ch->probe - 1) < 0.01f, ch_color(P.sel));
    seg_set(U.probe[1], fabsf(ch->probe - 10) < 0.01f, ch_color(P.sel));
    fmt_125(v, sizeof v, ch->scale, "V");
    set_text(U.vdiv.val, v);
    fmt_eng(v, sizeof v, ch->offset, "V", 3);
    set_text(U.pos.val, v);
    /* time and trigger */
    fmt_125(v, sizeof v, st->timebase, "s");
    set_text(U.tb.val, v);
    fmt_eng(v, sizeof v, st->trig_level, "V", 3);
    set_text(U.lvl.val, v);
    for (int c = 0; c < NCH; c++) seg_set(U.src[c], st->trig_source == c + 1, ch_color(c));
    seg_set(U.slope[0], !strcmp(st->trig_slope, "POS"), lv_color_hex(RGB_TRIG));
    seg_set(U.slope[1], !strcmp(st->trig_slope, "NEG"), lv_color_hex(RGB_TRIG));
    if (st->trig_level != U.last_level) {
        if (U.ticks > 3) U.trig_changed_ms = now_ms();
        U.last_level = st->trig_level;
    }
}

static void refresh_meas(const aos_scope_status_t *st)
{
    for (int c = 0; c < NCH; c++) {
        char a[40], b[40], s[96], x[24], y[24];
        float vpp, f, avg, rms;
        bool on = st->connected && st->ch[c].on;
        opa_if(U.mcard[c], on ? LV_OPA_COVER : LV_OPA_50);
        if (!on) {
            snprintf(a, sizeof a, "CH%d", c + 1);
            set_text(U.m_vpp[c], a);
            set_text(U.m_freq[c], "");
            set_text(U.m_sub[c], st->connected ? _("apagado") : "");
            tc_if(U.m_vpp[c], AOS_C_DIM);
            continue;
        }
        tc_if(U.m_vpp[c], AOS_C_TEXT);
        if (aos_scope_measure(c + 1, AOS_SCOPE_M_VPP, &vpp, NULL)) { fmt_eng(x, sizeof x, vpp, "V", 3); snprintf(a, sizeof a, "%spp", x); }
        else snprintf(a, sizeof a, "-- Vpp");
        if (aos_scope_measure(c + 1, AOS_SCOPE_M_FREQ, &f, NULL)) fmt_eng(b, sizeof b, f, "Hz", 4);
        else snprintf(b, sizeof b, "-- Hz");
        bool ha = aos_scope_measure(c + 1, AOS_SCOPE_M_VAVG, &avg, NULL);
        bool hr = aos_scope_measure(c + 1, AOS_SCOPE_M_VRMS, &rms, NULL);
        if (ha) fmt_eng(x, sizeof x, avg, "V", 3); else snprintf(x, sizeof x, "--");
        if (hr) fmt_eng(y, sizeof y, rms, "V", 3); else snprintf(y, sizeof y, "--");
        snprintf(s, sizeof s, "%s %s   %s %s", _("media"), x, "rms", y);
        set_text(U.m_vpp[c], a);
        set_text(U.m_freq[c], b);
        set_text(U.m_sub[c], s);
    }
}

/* -------------------------------------------------------------------------- */
/* The contract                                                                */
/* -------------------------------------------------------------------------- */

/* What LVGL spends on a frame while the tab shows, for the log. */
static void refr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) { U.refr_t0 = now_us(); return; }
    if (!U.refr_t0) return;
    uint32_t d = (uint32_t)(now_us() - U.refr_t0);
    U.refr_t0 = 0;
    U.refr_n++;
    U.refr_us += d;
    if (d > U.refr_max) U.refr_max = d;
}

void bench_scope_build(lv_obj_t *parent, int32_t w, int32_t h, bool landscape)
{
    bench_scope_destroy();
    aos_scope_start();
    memset(&U, 0, sizeof U);
    U.w = w;
    U.h = h;
    U.land = landscape;
    U.wave = malloc(sizeof(float) * NCH * AOS_SCOPE_POINTS);
    U.root = box(parent, w, h);
    build_all();
    U.was_connected = true;             /* so the first refresh sets the dimming either way */
    aos_scope_status_t st;
    aos_scope_status(&st);
    U.last_level = st.trig_level;
    if (!st.ch[P.sel].on) for (int c = 0; c < NCH; c++) if (st.ch[c].on) { P.sel = c; break; }
    refresh_controls(&st);
    refresh_meas(&st);
    conn_refresh(&st);
    redraw(&st);
    U.perf_t0 = now_ms();
    U.disp = lv_obj_get_display(U.root);
    if (U.disp) {
        lv_display_add_event_cb(U.disp, refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(U.disp, refr_cb, LV_EVENT_REFR_READY, NULL);
    }
    aos_scope_live(true);
}

void bench_scope_tick(void)
{
    if (!U.root) return;
    U.ticks++;
    aos_scope_status_t st;
    aos_scope_status(&st);
    refresh_controls(&st);
    conn_refresh(&st);
    if (U.ticks % 3 == 0) refresh_meas(&st);
    if (st.wave_seq != U.seen_seq && U.wave) {
        U.seen_seq = st.wave_seq;
        for (int c = 0; c < NCH; c++) {
            float xi;
            U.wave_n[c] = st.ch[c].on ? aos_scope_wave(c + 1, U.wave + c * AOS_SCOPE_POINTS, AOS_SCOPE_POINTS, &xi) : 0;
        }
    }
    uint32_t k = draw_key(&st);
    if (k != U.drawn_key) {
        U.drawn_key = k;
        redraw(&st);
    }
    shot_tick();
    uint32_t t = now_ms();
    if (t - U.perf_t0 >= 10000) {
        aos_hal_log("scope", "tab: %u redraws in %u ms, %u us each; LVGL %u frames, %u us avg, %u max; scope %.1f sets/s",
                    (unsigned)U.redraws, (unsigned)(t - U.perf_t0), (unsigned)(U.redraws ? U.draw_us / U.redraws : 0),
                    (unsigned)U.refr_n, (unsigned)(U.refr_n ? U.refr_us / U.refr_n : 0), (unsigned)U.refr_max, (double)st.fps);
        U.perf_t0 = t;
        U.redraws = U.draw_us = U.refr_n = U.refr_us = U.refr_max = 0;
    }
}

void bench_scope_destroy(void)
{
    if (!U.root) return;
    aos_scope_live(false);
    ovl_close();
    if (U.disp) lv_display_remove_event_cb_with_user_data(U.disp, refr_cb, NULL);
    free(U.wave);
    /* the objects are the parent's owner's to delete; the grid's pixels go with the canvas */
    memset(&U, 0, sizeof U);
}

bool bench_scope_back(void)
{
    if (!U.root) return false;
    if (U.ovl) { ovl_close(); return true; }
    if (P.conn_open) { P.conn_open = false; return true; }
    return false;
}
