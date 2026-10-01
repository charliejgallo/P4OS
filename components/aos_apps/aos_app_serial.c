/*
 * P4OS - Terminal: the serial monitor of the bench.
 *
 * It shows a channel of the capture service (aos_serial.c), which keeps
 * reading with the app closed; there are two, A and B, each with its port
 * and speed. Lines are coloured by ESP-IDF's log letter (E red, W amber, I
 * green, D/V grey); a line a trigger matched is lit amber and a chip's boot
 * blue, so a crash and the reset after it jump out of a long log. Drag the
 * text down to read back - the view stops following - and drag it to the
 * bottom, or tap "Final", to follow again.
 *
 * The tools, one tap each:
 *   hora       the time each line arrived, in front
 *   grabar     every line to <card>/logs/, with the date (the service does
 *              it: it goes on with the app closed)
 *   hexa       the bytes as they came, for protocols without lines
 *   filtro     only the lines that contain a text
 *   avisos     the triggers: texts that raise a notification or a beep
 *              when a line has them, for both channels
 * The bar below sends a line with CR LF, LF, CR or nothing after it.
 *
 * The view is not a label with the whole ring in it: it is one label per
 * visible row, refilled from the ring when something changes. That costs
 * the same with ten lines as with three thousand.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 64
#define FILTER_MAX 3000

static const uint32_t BAUDS[] = { 9600, 19200, 38400, 57600, 74880, 115200, 230400, 460800, 921600, 1500000, 2000000 };
#define N_BAUDS (sizeof BAUDS / sizeof BAUDS[0])

enum { EOL_CRLF, EOL_LF, EOL_CR, EOL_NONE, EOL_COUNT };
static const char *const EOL_NAME[EOL_COUNT] = { "CR LF", "LF", "CR", N_("nada") };
static const char *const EOL_SEQ[EOL_COUNT] = { "\r\n", "\n", "\r", "" };

static const char *const PRESETS[] = { "Guru Meditation", "abort()", "Brownout", "rst:0x", "Backtrace", "E (", "WDT" };

/* what survives closing the app and turning the screen */
static struct {
    int ch;
    bool stamps, hex;
    int eol;
    char filter[40];
    bool loaded;
} S;

typedef struct {
    lv_obj_t *root, *bar, *port_dd, *baud_dd, *connect, *connect_lbl, *status, *follow_btn;
    lv_obj_t *seg[2], *rec_btn, *hex_btn, *filter_btn, *filter_lbl, *trig_btn, *trig_lbl, *stamp_btn, *eol_lbl;
    lv_obj_t *view, *rows[MAX_ROWS];
    lv_obj_t *ta, *kb, *overlay, *sheet_ta, *sheet_kb, *trig_list;
    int32_t W, H;
    int nrows, row_h, hex_bpr;
    uint32_t top;           /* first position shown, when not following */
    uint64_t hex_top;       /* first hex row, when not following */
    bool follow;
    uint32_t shown_lines, shown_top;
    uint64_t shown_raw;
    int32_t drag_acc;
    lv_timer_t *timer;
    /* the filter's index: positions -> line numbers */
    uint32_t *fidx;
    int nf;
    uint32_t scanned;
    /* the hex view's: the rows where the pattern starts */
    uint64_t *hidx;
    int nh;
    uint64_t hscan_total;
    uint8_t *hbuf;
    void (*text_done)(const char *);
} term_t;

static term_t T;

static void refresh(bool force);

static void prefs_load(void)
{
    if (S.loaded) return;
    S.loaded = true;
    int32_t v;
    if (aos_hal_pref_get_i32("ser_eol", &v) && v >= 0 && v < EOL_COUNT) S.eol = v;
    if (aos_hal_pref_get_i32("ser_ch", &v) && (v == 0 || v == 1)) S.ch = v;
}

static const char *key_port(int ch) { return ch ? "ser_port1" : "ser_port"; }
static const char *key_baud(int ch) { return ch ? "ser_baud1" : "ser_baud"; }

static lv_color_t level_color(const char *s)
{
    if (s[0] && s[1] == ' ' && s[2] == '(') {
        switch (s[0]) {
        case 'E': return lv_color_hex(0xFF6B6B);
        case 'W': return lv_color_hex(0xFFC857);
        case 'I': return lv_color_hex(0x7BD88F);
        case 'D': case 'V': return lv_color_hex(0x8A93A3);
        default: break;
        }
    }
    return lv_color_hex(0xE6EDF3);
}

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

/* ---- the filter's index ---- */

static void filter_reset(void)
{
    T.nf = 0;
    T.scanned = 0;
    T.nh = 0;
    T.hscan_total = UINT64_MAX;
    T.follow = true;
}

static void filter_scan(const aos_serial_stat_t *st)
{
    if (!T.fidx) T.fidx = malloc(FILTER_MAX * sizeof *T.fidx);
    if (!T.fidx) return;
    /* forget what left the ring */
    int drop = 0;
    while (drop < T.nf && T.fidx[drop] < st->first) drop++;
    if (drop) { memmove(T.fidx, T.fidx + drop, (size_t)(T.nf - drop) * sizeof *T.fidx); T.nf -= drop; }
    if (T.scanned < st->first) T.scanned = st->first;
    char txt[AOS_SERIAL_LINE_MAX];
    for (; T.scanned < st->lines; T.scanned++) {
        if (aos_serial_line(S.ch, T.scanned, txt, sizeof txt, NULL) < 0) continue;
        if (!contains_nocase(txt, S.filter)) continue;
        if (T.nf == FILTER_MAX) { memmove(T.fidx, T.fidx + 1, (FILTER_MAX - 1) * sizeof *T.fidx); T.nf--; }
        T.fidx[T.nf++] = T.scanned;
    }
}

/* The filter in the hex view: bytes when written as bytes ("DE AD BE EF",
 * pairs separated by spaces, or "0xDEADBEEF"), otherwise text, case ignored. */
#define HEX_HITS_MAX 2048
static int hex_pattern(const char *f, uint8_t *out, int max, bool *nocase)
{
    int n = 0;
    *nocase = false;
    bool hex0x = f[0] == '0' && (f[1] == 'x' || f[1] == 'X');
    bool pairs = !hex0x && strchr(f, ' ') != NULL;             /* a lone "ab" is text */
    for (const char *q = f; *q && pairs; ) {                     /* "DE AD": every word two hex digits */
        while (*q == ' ') q++;
        if (!*q) break;
        int k = 0;
        while (q[k] && q[k] != ' ') k++;
        if (k != 2 || !isxdigit((unsigned char)q[0]) || !isxdigit((unsigned char)q[1])) pairs = false;
        q += k;
    }
    if (hex0x) {
        const char *p = f + 2;
        size_t l = strlen(p);
        bool ok = l >= 2 && l % 2 == 0;
        for (size_t i = 0; i < l && ok; i++) ok = isxdigit((unsigned char)p[i]);
        if (ok) {
            for (size_t i = 0; i + 1 < l && n < max; i += 2) { char h[3] = { p[i], p[i + 1], 0 }; out[n++] = (uint8_t)strtoul(h, NULL, 16); }
            return n;
        }
    } else if (pairs) {
        for (const char *q = f; *q && n < max; ) {
            while (*q == ' ') q++;
            if (!*q) break;
            char h[3] = { q[0], q[1], 0 };
            out[n++] = (uint8_t)strtoul(h, NULL, 16);
            q += 2;
        }
        return n;
    }
    *nocase = true;
    for (; *f && n < max; f++) out[n++] = (uint8_t)*f;
    return n;
}

static void hex_scan(const aos_serial_stat_t *st)
{
    if (st->raw_total == T.hscan_total) return;
    T.hscan_total = st->raw_total;
    T.nh = 0;
    if (!T.hidx) T.hidx = malloc(HEX_HITS_MAX * sizeof *T.hidx);
    if (!T.hbuf) T.hbuf = malloc(65536);
    if (!T.hidx || !T.hbuf) return;
    uint8_t pat[40];
    bool nocase;
    int pn = hex_pattern(S.filter, pat, sizeof pat, &nocase);
    if (!pn) return;
    uint64_t oldest = st->raw_total > 65536 ? st->raw_total - 65536 : 0, next = 0;
    int n = aos_serial_raw(S.ch, oldest, T.hbuf, 65536, &next);
    if (n <= 0) return;
    uint64_t base = next - (uint64_t)n, last_row = UINT64_MAX;
    for (int i = 0; i + pn <= n; i++) {
        int k = 0;
        while (k < pn && (nocase ? tolower(T.hbuf[i + k]) == tolower(pat[k]) : T.hbuf[i + k] == pat[k])) k++;
        if (k < pn) continue;
        uint64_t row = (base + (uint64_t)i) / (uint64_t)T.hex_bpr;
        if (row == last_row) continue;
        last_row = row;
        if (T.nh == HEX_HITS_MAX) { memmove(T.hidx, T.hidx + 1, (HEX_HITS_MAX - 1) * sizeof *T.hidx); T.nh--; }
        T.hidx[T.nh++] = row;
    }
}

/* The hex view's positions: rows of the ring, or entries of the hits. */
static void hex_positions(const aos_serial_stat_t *st, uint64_t *lo, uint64_t *hi)
{
    if (S.filter[0]) { hex_scan(st); *lo = 0; *hi = (uint64_t)T.nh; return; }
    uint64_t total = st->raw_total, oldest = total > 65536 ? total - 65536 : 0;
    *lo = oldest / (uint64_t)T.hex_bpr;
    *hi = (total + (uint64_t)T.hex_bpr - 1) / (uint64_t)T.hex_bpr;
}

/* positions: line numbers without a filter, entries of the index with one */
static void positions(const aos_serial_stat_t *st, uint32_t *lo, uint32_t *hi)
{
    if (S.filter[0]) { *lo = 0; *hi = (uint32_t)T.nf; }
    else { *lo = st->first; *hi = st->lines; }
}

static uint32_t line_at(uint32_t pos) { return S.filter[0] ? T.fidx[pos] : pos; }

/* ---- drawing ---- */

/* What a line can show: printable ASCII and well-formed UTF-8 stay; a
 * stray byte of a binary protocol or a control character becomes "·"
 * instead of a font's empty box. */
static void sanitize(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p && k + 4 < cap) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0;
        for (int i = 1; i < len && ok; i++) ok = (p[i] & 0xC0) == 0x80;
        if (ok && len == 2 && *p < 0xC2) ok = false;                  /* overlong */
        if (ok && len == 1 && (*p < 0x20 || *p == 0x7F)) ok = *p == '\t';
        if (ok && len > 1) {                                           /* and one the mono font has */
            uint32_t cp = len == 2 ? (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F)
                        : len == 3 ? (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F) : 0;
            ok = (cp >= 0xA0 && cp <= 0xFF) || cp == 0x2022 || cp == 0x2026 || (cp >= 0x2500 && cp <= 0x259F);
            if (!ok) { out[k++] = (char)0xC2; out[k++] = (char)0xB7; p += len; continue; }
        }
        if (!ok) { out[k++] = (char)0xC2; out[k++] = (char)0xB7; p++; continue; }
        if (len == 1 && *p == '\t') { out[k++] = ' '; p++; continue; }
        for (int i = 0; i < len; i++) out[k++] = (char)p[i];
        p += len;
    }
    out[k] = 0;
}

static void draw_text(const aos_serial_stat_t *st, bool force)
{
    if (S.filter[0]) filter_scan(st);
    uint32_t lo, hi;
    positions(st, &lo, &hi);
    uint32_t top;
    if (T.follow) top = hi - lo > (uint32_t)T.nrows ? hi - T.nrows : lo;
    else {
        if (T.top < lo) T.top = lo;
        top = T.top;
    }
    if (!force && st->lines == T.shown_lines && top == T.shown_top) return;
    T.shown_lines = st->lines;
    T.shown_top = top;
    char line[2 * AOS_SERIAL_LINE_MAX + 16];
    for (int r = 0; r < T.nrows; r++) {
        uint32_t pos = top + (uint32_t)r;
        uint32_t t = 0;
        uint8_t f = 0;
        char txt[AOS_SERIAL_LINE_MAX];
        int n = pos < hi ? aos_serial_line_ex(S.ch, line_at(pos), txt, sizeof txt, &t, &f) : -1;
        lv_obj_t *l = T.rows[r];
        if (n < 0) {
            lv_label_set_text(l, "");
            lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
            continue;
        }
        char clean[2 * AOS_SERIAL_LINE_MAX];
        sanitize(txt, clean, sizeof clean);
        if (S.stamps) snprintf(line, sizeof line, "%5u.%03u %s", (unsigned)(t / 1000), (unsigned)(t % 1000), clean);
        else snprintf(line, sizeof line, "%s", clean);
        lv_label_set_text(l, line);
        bool trig = f & AOS_SERIAL_F_TRIGGER, boot = f & AOS_SERIAL_F_BOOT;
        lv_obj_set_style_text_color(l, trig ? lv_color_hex(0xFFE08A) : level_color(txt), 0);
        lv_obj_set_style_bg_color(l, trig ? lv_color_hex(0x5C3D00) : lv_color_hex(0x10305A), 0);
        lv_obj_set_style_bg_opa(l, trig || boot ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void draw_hex(const aos_serial_stat_t *st, bool force)
{
    int bpr = T.hex_bpr;
    uint64_t total = st->raw_total, lo, hi;
    hex_positions(st, &lo, &hi);
    uint64_t top;
    if (T.follow) top = hi - lo > (uint64_t)T.nrows ? hi - T.nrows : lo;
    else {
        if (T.hex_top < lo) T.hex_top = lo;
        top = T.hex_top;
    }
    if (!force && total == T.shown_raw && (uint32_t)top == T.shown_top) return;
    T.shown_raw = total;
    T.shown_top = (uint32_t)top;
    for (int r = 0; r < T.nrows; r++) {
        uint64_t pos = top + (uint64_t)r;
        lv_obj_t *l = T.rows[r];
        lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
        if (pos >= hi) { lv_label_set_text(l, ""); continue; }
        uint64_t row = S.filter[0] ? T.hidx[pos] : pos;
        uint8_t b[16];
        uint64_t next = 0;
        int n = aos_serial_raw(S.ch, row * bpr, b, bpr, &next);
        if (n <= 0 || next - (uint64_t)n != row * bpr) { lv_label_set_text(l, ""); continue; }   /* it left the ring */
        char s[128];
        int k = snprintf(s, sizeof s, "%06lX ", (unsigned long)((row * bpr) & 0xFFFFFF));
        for (int i = 0; i < bpr; i++) k += snprintf(s + k, sizeof s - (size_t)k, i < n ? " %02X" : "   ", i < n ? b[i] : 0);
        k += snprintf(s + k, sizeof s - (size_t)k, "  ");
        for (int i = 0; i < n && k < (int)sizeof s - 1; i++) s[k++] = b[i] >= 0x20 && b[i] < 0x7F ? (char)b[i] : '.';
        s[k] = 0;
        lv_label_set_text(l, s);
        if (S.filter[0]) {                  /* a hit starts in this row */
            lv_obj_set_style_text_color(l, lv_color_hex(0xFFE08A), 0);
            lv_obj_set_style_bg_color(l, lv_color_hex(0x3A2A00), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(l, lv_color_hex(0xB8C4D0), 0);
        }
    }
}

static void refresh(bool force)
{
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    if (S.hex) draw_hex(&st, force);
    else draw_text(&st, force);

    if (st.running) {
        char rec[48] = "";
        if (st.recording) snprintf(rec, sizeof rec, "  ·  %s %llu KB", _("grabando"), (unsigned long long)(st.rec_bytes / 1024));
        lv_label_set_text_fmt(T.status, "%s  ·  %u  ·  %u %s  ·  %llu B%s%s", st.desc, (unsigned)st.baud,
                              (unsigned)st.lines, _("líneas"), (unsigned long long)st.rx_bytes, rec,
                              S.filter[0] ? "" : "");
    } else {
        lv_label_set_text(T.status, _("Sin conexión: elegí el puerto y la velocidad"));
    }
    lv_label_set_text(T.connect_lbl, st.running ? _("Detener") : _("Conectar"));
    lv_obj_set_style_bg_color(T.connect, st.running ? AOS_C_RED : AOS_C_GREEN, 0);
    lv_obj_set_flag(T.follow_btn, LV_OBJ_FLAG_HIDDEN, T.follow);
    lv_obj_set_state(T.rec_btn, LV_STATE_CHECKED, st.recording);
    lv_obj_set_style_text_color(lv_obj_get_child(T.rec_btn, 0), st.recording ? lv_color_white() : AOS_C_RED, 0);
    lv_obj_set_state(T.hex_btn, LV_STATE_CHECKED, S.hex);
    lv_obj_set_state(T.stamp_btn, LV_STATE_CHECKED, S.stamps);
    lv_obj_set_state(T.filter_btn, LV_STATE_CHECKED, S.filter[0] != 0);
    if (S.filter[0]) lv_label_set_text_fmt(T.filter_lbl, "%s", S.filter);
    else lv_label_set_text(T.filter_lbl, _("Filtro"));
    uint32_t hits = 0;
    for (int i = 0; i < aos_serial_trigger_count(); i++) {
        aos_serial_trigger_t t;
        if (aos_serial_trigger_get(i, &t)) hits += t.hits;
    }
    if (hits) lv_label_set_text_fmt(T.trig_lbl, "%s %u", _("Avisos"), (unsigned)hits);
    else lv_label_set_text(T.trig_lbl, _("Avisos"));
    for (int c = 0; c < 2; c++) {
        bool on = c == S.ch;
        lv_obj_set_style_bg_color(T.seg[c], on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(T.seg[c], 0), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    lv_label_set_text(T.eol_lbl, aos_tr(EOL_NAME[S.eol]));
}

static void timer_cb(lv_timer_t *t) { refresh(false); }

/* ---- the connection ---- */

static void load_port_baud(void)
{
    char saved[AOS_IO_PORT_NAME_MAX] = "";
    snprintf(saved, sizeof saved, "%s", S.ch ? "uart.a" : "uart.b");
    aos_hal_pref_get_str(key_port(S.ch), saved, sizeof saved);
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    if (st.running) snprintf(saved, sizeof saved, "%s", st.port);
    int sel = lv_dropdown_get_option_index(T.port_dd, saved);
    if (sel >= 0) lv_dropdown_set_selected(T.port_dd, (uint32_t)sel);
    int32_t baud = 115200;
    aos_hal_pref_get_i32(key_baud(S.ch), &baud);
    if (st.running) baud = (int32_t)st.baud;
    for (size_t i = 0; i < N_BAUDS; i++) if (BAUDS[i] == (uint32_t)baud) lv_dropdown_set_selected(T.baud_dd, (uint32_t)i);
}

static void connect_cb(lv_event_t *e)
{
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    if (st.running) {
        aos_serial_stop(S.ch);
    } else {
        char port[AOS_IO_PORT_NAME_MAX];
        lv_dropdown_get_selected_str(T.port_dd, port, sizeof port);
        uint32_t baud = BAUDS[lv_dropdown_get_selected(T.baud_dd)];
        if (!aos_serial_start(S.ch, port, baud)) {
            const aos_io_port_t *p = aos_io_port_find(port);
            const char *holder = p ? aos_io_owner(p->pins[0]) : NULL;
            char t[80];
            if (holder) snprintf(t, sizeof t, _("%s lo tiene %s"), port, holder);
            else snprintf(t, sizeof t, "%s", _("No se pudo abrir el puerto"));
            aos_ui_toast(t, 2500);
        }
        aos_hal_pref_set_str(key_port(S.ch), port);
        aos_hal_pref_set_i32(key_baud(S.ch), (int32_t)baud);
        T.follow = true;
        filter_reset();
    }
    refresh(true);
}

static void baud_cb(lv_event_t *e)
{
    /* changing the speed of a running capture restarts it on the same port */
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    uint32_t baud = BAUDS[lv_dropdown_get_selected(T.baud_dd)];
    aos_hal_pref_set_i32(key_baud(S.ch), (int32_t)baud);
    if (st.running) {
        aos_serial_stop(S.ch);
        aos_serial_start(S.ch, st.port, baud);
    }
    refresh(true);
}

static void channel_cb(lv_event_t *e)
{
    S.ch = (int)(intptr_t)lv_event_get_user_data(e);
    aos_hal_pref_set_i32("ser_ch", S.ch);
    filter_reset();
    load_port_baud();
    refresh(true);
}

/* ---- the tools ---- */

static void clear_cb(lv_event_t *e) { aos_serial_clear(S.ch); filter_reset(); refresh(true); }
static void stamps_cb(lv_event_t *e) { S.stamps = !S.stamps; refresh(true); }
static void follow_cb(lv_event_t *e) { T.follow = true; refresh(true); }
static void hex_cb(lv_event_t *e) { S.hex = !S.hex; T.follow = true; T.hscan_total = UINT64_MAX; refresh(true); }

static void rec_cb(lv_event_t *e)
{
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    if (!st.running) { aos_ui_toast(_("Conectá un puerto primero"), 1800); return; }
    if (!aos_serial_record(S.ch, !st.recording)) { aos_ui_toast(_("Hace falta la tarjeta"), 1800); return; }
    aos_ui_toast(st.recording ? _("Grabación guardada en /logs") : _("Grabando en /logs"), 1800);
    refresh(true);
}

static void sheet_close(void)
{
    if (T.overlay) lv_obj_delete(T.overlay);
    T.overlay = T.sheet_ta = T.sheet_kb = T.trig_list = NULL;
}

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[64];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(T.sheet_ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? T.text_done : NULL;
    sheet_close();
    if (done) done(v);
}

static void text_entry(const char *title, const char *value, void (*done)(const char *))
{
    sheet_close();
    T.text_done = done;
    T.overlay = lv_obj_create(T.root);
    lv_obj_remove_style_all(T.overlay);
    lv_obj_set_size(T.overlay, T.W, T.H);
    lv_obj_set_style_bg_color(T.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(T.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(T.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tl = aos_label(T.overlay, title, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(tl, T.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 30);
    T.sheet_ta = lv_textarea_create(T.overlay);
    lv_textarea_set_one_line(T.sheet_ta, true);
    lv_textarea_set_max_length(T.sheet_ta, AOS_SERIAL_TRIGGER_MAX - 1);
    lv_textarea_set_text(T.sheet_ta, value);
    lv_obj_set_size(T.sheet_ta, T.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(T.sheet_ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 100);
    lv_obj_set_style_text_font(T.sheet_ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(T.sheet_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(T.sheet_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(T.sheet_ta, 0, 0);
    lv_obj_set_style_radius(T.sheet_ta, 20, 0);
    lv_obj_set_style_pad_hor(T.sheet_ta, 24, 0);
    lv_obj_set_style_pad_ver(T.sheet_ta, 24, 0);
    T.sheet_kb = lv_keyboard_create(T.overlay);
    lv_obj_set_size(T.sheet_kb, T.W, T.W > T.H ? T.H / 2 : T.H * 2 / 5);
    lv_obj_align(T.sheet_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(T.sheet_kb, aos_font_body);
    lv_keyboard_set_textarea(T.sheet_kb, T.sheet_ta);
    lv_obj_add_event_cb(T.sheet_kb, text_cb, LV_EVENT_ALL, NULL);
}

static void filter_done(const char *v)
{
    snprintf(S.filter, sizeof S.filter, "%s", v);
    filter_reset();
    refresh(true);
}

static void filter_cb(lv_event_t *e)
{
    if (S.filter[0]) { S.filter[0] = 0; filter_reset(); refresh(true); return; }   /* a tap on an active filter clears it */
    if (S.hex) text_entry(_("Mostrar sólo las filas con… (texto, o bytes como «DE AD»)"), "", filter_done);
    else text_entry(_("Mostrar sólo las líneas con…"), "", filter_done);
}

/* ---- the triggers' sheet ---- */

static void trig_build(void);

static void trig_flag_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int i = v >> 4, bit = v & 0xF;
    aos_serial_trigger_t t;
    if (!aos_serial_trigger_get(i, &t)) return;
    aos_serial_trigger_set(i, (uint8_t)(t.flags ^ bit));
    trig_build();
}

static void trig_del_cb(lv_event_t *e)
{
    aos_serial_trigger_remove((int)(intptr_t)lv_event_get_user_data(e));
    trig_build();
}

static void trig_add_done(const char *v)
{
    if (v[0] && aos_serial_trigger_add(v, AOS_SERIAL_T_NOTIFY) < 0) aos_ui_toast(_("Ya hay 8 avisos"), 1800);
    trig_build();
}

static void trig_add_cb(lv_event_t *e) { text_entry(_("Avisar cuando aparezca…"), "", trig_add_done); }

static int s_trig_edit = -1;

static void trig_edit_done(const char *v)
{
    if (v[0]) aos_serial_trigger_rename(s_trig_edit, v);
    trig_build();
}

static void trig_edit_cb(lv_event_t *e)
{
    s_trig_edit = (int)(intptr_t)lv_event_get_user_data(e);
    aos_serial_trigger_t t;
    if (!aos_serial_trigger_get(s_trig_edit, &t)) return;
    text_entry(_("Avisar cuando aparezca…"), t.text, trig_edit_done);
}

static void trig_preset_cb(lv_event_t *e)
{
    aos_serial_trigger_add(lv_event_get_user_data(e), AOS_SERIAL_T_NOTIFY);
    trig_build();
}

static void trig_close_cb(lv_event_t *e) { sheet_close(); refresh(true); }

static lv_obj_t *mini(lv_obj_t *parent, const char *text, bool on, lv_color_t on_color, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_bg_color(b, on ? on_color : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(b, text, aos_font_small, on ? lv_color_white() : AOS_C_TEXT));
    return b;
}

static void trig_build(void)
{
    if (!T.overlay) {
        T.overlay = lv_obj_create(T.root);
        lv_obj_remove_style_all(T.overlay);
        lv_obj_set_size(T.overlay, T.W, T.H);
        lv_obj_set_style_bg_color(T.overlay, lv_color_hex(0x0B0E12), 0);
        lv_obj_set_style_bg_opa(T.overlay, LV_OPA_COVER, 0);
        lv_obj_add_flag(T.overlay, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_clean(T.overlay);
    int32_t w = T.W > T.H ? 820 : T.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lv_obj_create(T.overlay);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, w, T.H);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_set_style_pad_bottom(col, 30, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, w, 100);
    lv_obj_align(aos_label(head, _("Avisos"), aos_font_large, AOS_C_TEXT), LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_t *done = mini(head, _("Listo"), true, AOS_C_ACCENT, trig_close_cb, NULL);
    lv_obj_align(done, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *note = aos_label(col, _("Cada línea de los dos canales se compara con estos textos, sin importar mayúsculas. Una coincidencia se marca en ámbar y se cuenta; con aviso o pitido, además avisa, como mucho una vez cada 10 segundos."),
                               aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(note, w);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    int n = aos_serial_trigger_count();
    for (int i = 0; i < n; i++) {
        aos_serial_trigger_t t;
        if (!aos_serial_trigger_get(i, &t)) continue;
        lv_obj_t *r = lv_obj_create(card);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), AOS_UI_ROW_H);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        if (i) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);       /* a tap on the row edits the text */
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, trig_edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(r);
        lv_obj_set_style_text_font(l, &aos_mono_22, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFE08A), 0);
        lv_label_set_text(l, t.text);
        lv_obj_set_width(l, w - 36 - 330);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, -12);
        lv_obj_t *h = aos_label(r, "", aos_font_tiny, AOS_C_DIM);
        if (t.hits == 1) lv_label_set_text(h, _("1 vez"));
        else lv_label_set_text_fmt(h, _("%u veces"), (unsigned)t.hits);
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 0, 22);
        lv_obj_t *btns = lv_obj_create(r);
        lv_obj_remove_style_all(btns);
        lv_obj_set_size(btns, LV_SIZE_CONTENT, 60);
        lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(btns, 8, 0);
        lv_obj_align(btns, LV_ALIGN_RIGHT_MID, 0, 0);
        mini(btns, _("Aviso"), t.flags & AOS_SERIAL_T_NOTIFY, AOS_C_ACCENT, trig_flag_cb, (void *)(intptr_t)(i << 4 | AOS_SERIAL_T_NOTIFY));
        mini(btns, _("Pitido"), t.flags & AOS_SERIAL_T_BEEP, AOS_C_ORANGE, trig_flag_cb, (void *)(intptr_t)(i << 4 | AOS_SERIAL_T_BEEP));
        lv_obj_t *x = mini(btns, "", false, AOS_C_RED, trig_del_cb, (void *)(intptr_t)i);
        lv_obj_center(aos_label(x, AOS_SYM_CLOSE, &aos_sym_28, AOS_C_DIM));
    }
    if (!n) {
        lv_obj_t *e = aos_label(card, _("Sin avisos."), aos_font_body, AOS_C_DIM);
        lv_obj_set_style_pad_all(e, 22, 0);
    }
    if (n < AOS_SERIAL_TRIGGERS) {
        lv_obj_t *add = mini(col, _("Agregar un texto…"), true, AOS_C_ACCENT, trig_add_cb, NULL);
        (void)add;
        lv_obj_t *t = aos_label(col, _("SUGERENCIAS"), aos_font_caption, AOS_C_DIM);
        lv_obj_set_style_pad_left(t, 8, 0);
        lv_obj_t *pr = lv_obj_create(col);
        lv_obj_remove_style_all(pr);
        lv_obj_set_size(pr, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(pr, 10, 0);
        for (size_t k = 0; k < sizeof PRESETS / sizeof PRESETS[0]; k++) {
            bool have = false;
            for (int i = 0; i < n && !have; i++) {
                aos_serial_trigger_t tt;
                have = aos_serial_trigger_get(i, &tt) && !strcmp(tt.text, PRESETS[k]);
            }
            if (!have) mini(pr, PRESETS[k], false, AOS_C_ACCENT, trig_preset_cb, (void *)PRESETS[k]);
        }
    }
}

static void trig_cb(lv_event_t *e) { sheet_close(); trig_build(); }

/* ---- the view ---- */

static void view_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) { T.drag_acc = 0; return; }
    if (c != LV_EVENT_PRESSING) return;
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    T.drag_acc += v.y;
    int steps = T.drag_acc / T.row_h;
    if (!steps) return;
    T.drag_acc -= steps * T.row_h;
    aos_serial_stat_t st;
    aos_serial_stat(S.ch, &st);
    if (S.hex) {
        uint64_t plo, phi;
        hex_positions(&st, &plo, &phi);
        int64_t lo = (int64_t)plo, last = (int64_t)phi - T.nrows;
        if (T.follow) { T.hex_top = T.shown_top; T.follow = false; }
        int64_t top = (int64_t)T.hex_top - steps;
        if (top < lo) top = lo;
        if (top >= last) { top = last; T.follow = true; }
        T.hex_top = (uint64_t)(top < 0 ? 0 : top);
    } else {
        uint32_t lo, hi;
        positions(&st, &lo, &hi);
        if (T.follow) { T.top = T.shown_top; T.follow = false; }
        int64_t top = (int64_t)T.top - steps;          /* drag down = older lines */
        int64_t last = (int64_t)hi - T.nrows;
        if (top < (int64_t)lo) top = lo;
        if (top >= last) { top = last; T.follow = true; }
        T.top = (uint32_t)(top < 0 ? 0 : top);
    }
    refresh(true);
}

/* ---- sending ---- */

static void send_cb(lv_event_t *e)
{
    const char *txt = lv_textarea_get_text(T.ta);
    char buf[260];
    int n = snprintf(buf, sizeof buf, "%s%s", txt, EOL_SEQ[S.eol]);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    if (aos_serial_send(S.ch, buf, n) < 0) aos_ui_toast(_("Conectá un puerto primero"), 1800);
    lv_textarea_set_text(T.ta, "");
}

static void eol_cb(lv_event_t *e)
{
    S.eol = (S.eol + 1) % EOL_COUNT;
    aos_hal_pref_set_i32("ser_eol", S.eol);
    refresh(true);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED) { lv_keyboard_set_textarea(T.kb, T.ta); lv_obj_remove_flag(T.kb, LV_OBJ_FLAG_HIDDEN); }
    else if (c == LV_EVENT_DEFOCUSED || c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        lv_obj_add_flag(T.kb, LV_OBJ_FLAG_HIDDEN);
        if (c == LV_EVENT_READY) send_cb(e);
    }
}

/* ---- building ---- */

static lv_obj_t *tool_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb, void *ud)
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
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, 200, 0);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int32_t w)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_size(d, w, 72);
    lv_obj_set_style_text_font(d, aos_font_small, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 20, 0);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(list, aos_font_small, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    return d;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    prefs_load();
    uint32_t *fidx = T.fidx;
    uint64_t *hidx = T.hidx;
    uint8_t *hbuf = T.hbuf;
    memset(&T, 0, sizeof T);
    T.fidx = fidx;
    T.hidx = hidx;
    T.hbuf = hbuf;
    T.hscan_total = UINT64_MAX;         /* the row width may have changed with the turn */
    T.follow = true;
    T.root = root;
    T.W = lv_obj_get_width(root);
    T.H = lv_obj_get_height(root);
    const int32_t pad = 16;
    bool land = T.W > T.H;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E12), 0);

    /* the bar: connection, then the tools; it wraps as the width asks */
    T.bar = lv_obj_create(root);
    lv_obj_remove_style_all(T.bar);
    lv_obj_set_size(T.bar, T.W - 2 * pad, LV_SIZE_CONTENT);
    lv_obj_set_pos(T.bar, pad, 8);
    lv_obj_set_flex_flow(T.bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(T.bar, 10, 0);
    lv_obj_remove_flag(T.bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int c = 0; c < 2; c++) {
        lv_obj_t *s = lv_obj_create(T.bar);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 64, 72);
        lv_obj_set_style_radius(s, 20, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, channel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
        lv_obj_center(aos_label(s, c ? "B" : "A", aos_font_body, AOS_C_TEXT));
        T.seg[c] = s;
    }
    char ports[160] = "";
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        if (ports[0]) strlcat(ports, "\n", sizeof ports);
        strlcat(ports, p->name, sizeof ports);
    }
    T.port_dd = dropdown(T.bar, ports[0] ? ports : "uart.b", 150);
    char bauds[160] = "";
    for (size_t i = 0; i < N_BAUDS; i++) {
        char b[12];
        snprintf(b, sizeof b, "%s%u", i ? "\n" : "", (unsigned)BAUDS[i]);
        strlcat(bauds, b, sizeof bauds);
    }
    T.baud_dd = dropdown(T.bar, bauds, 170);
    lv_obj_add_event_cb(T.baud_dd, baud_cb, LV_EVENT_VALUE_CHANGED, NULL);
    T.connect = tool_btn(T.bar, NULL, _("Conectar"), connect_cb, NULL);
    T.connect_lbl = lv_obj_get_child(T.connect, 0);
    lv_obj_set_flex_grow(T.connect, 1);
    T.stamp_btn = tool_btn(T.bar, AOS_SYM_CLOCK_OUTLINE, NULL, stamps_cb, NULL);
    if (!land) lv_obj_add_flag(T.stamp_btn, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    T.rec_btn = tool_btn(T.bar, AOS_SYM_RECORD_REC, NULL, rec_cb, NULL);
    lv_obj_set_style_bg_color(T.rec_btn, AOS_C_RED, LV_STATE_CHECKED);
    T.hex_btn = tool_btn(T.bar, NULL, _("Hexa"), hex_cb, NULL);
    T.filter_btn = tool_btn(T.bar, AOS_SYM_MAGNIFY, _("Filtro"), filter_cb, NULL);
    T.filter_lbl = lv_obj_get_child(T.filter_btn, 1);
    T.trig_btn = tool_btn(T.bar, AOS_SYM_BELL_OUTLINE, _("Avisos"), trig_cb, NULL);
    T.trig_lbl = lv_obj_get_child(T.trig_btn, 1);
    tool_btn(T.bar, AOS_SYM_DELETE, NULL, clear_cb, NULL);
    lv_obj_update_layout(T.bar);
    int32_t bar_h = lv_obj_get_height(T.bar);

    T.status = lv_label_create(root);
    lv_obj_set_style_text_font(T.status, aos_font_caption, 0);
    lv_obj_set_style_text_color(T.status, AOS_C_DIM, 0);
    lv_obj_set_width(T.status, T.W - 2 * pad);
    lv_label_set_long_mode(T.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(T.status, pad, 8 + bar_h + 8);

    /* the view: one label per row */
    const int32_t send_h = 80, view_y = 8 + bar_h + 42;
    int32_t view_h = T.H - view_y - send_h - 16;
    T.view = lv_obj_create(root);
    lv_obj_remove_style_all(T.view);
    lv_obj_set_pos(T.view, pad, view_y);
    lv_obj_set_size(T.view, T.W - 2 * pad, view_h);
    lv_obj_set_style_bg_color(T.view, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(T.view, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(T.view, 16, 0);
    lv_obj_remove_flag(T.view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(T.view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(T.view, view_cb, LV_EVENT_ALL, NULL);
    const lv_font_t *mono = &aos_mono_18;
    T.row_h = lv_font_get_line_height(mono) + 2;
    T.nrows = LV_MIN((view_h - 16) / T.row_h, MAX_ROWS);
    T.hex_bpr = T.W - 2 * pad > 900 ? 16 : 8;
    for (int r = 0; r < T.nrows; r++) {
        lv_obj_t *l = lv_label_create(T.view);
        lv_obj_set_style_text_font(l, mono, 0);
        lv_obj_set_size(l, T.W - 2 * pad - 12, T.row_h);
        lv_obj_set_style_pad_hor(l, 4, 0);
        lv_obj_set_style_radius(l, 4, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(l, "");
        lv_obj_set_pos(l, 6, 8 + r * T.row_h);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        T.rows[r] = l;
    }
    T.follow_btn = tool_btn(root, NULL, _("Final"), follow_cb, NULL);
    lv_obj_align_to(T.follow_btn, T.view, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(T.follow_btn, AOS_C_ACCENT, 0);

    /* send bar: the line ending, the text, send */
    lv_obj_t *eol = tool_btn(root, NULL, "CR LF", eol_cb, NULL);
    lv_obj_set_size(eol, 120, 72);
    lv_obj_set_pos(eol, pad, T.H - send_h);
    T.eol_lbl = lv_obj_get_child(eol, 0);
    T.ta = lv_textarea_create(root);
    lv_textarea_set_one_line(T.ta, true);
    lv_textarea_set_placeholder_text(T.ta, _("Enviar al puerto…"));
    lv_obj_set_size(T.ta, T.W - 2 * pad - 120 - 144 - 20, 72);
    lv_obj_set_pos(T.ta, pad + 130, T.H - send_h);
    lv_obj_set_style_text_font(T.ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(T.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(T.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(T.ta, 0, 0);
    lv_obj_set_style_radius(T.ta, 20, 0);
    lv_obj_add_event_cb(T.ta, ta_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *send = tool_btn(root, NULL, _("Enviar"), send_cb, NULL);
    lv_obj_set_size(send, 144, 72);
    lv_obj_set_pos(send, T.W - pad - 144, T.H - send_h);

    T.kb = lv_keyboard_create(root);
    lv_obj_set_size(T.kb, T.W, land ? T.H / 2 : T.H / 3);
    lv_obj_align(T.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(T.kb, aos_font_small);
    lv_obj_add_flag(T.kb, LV_OBJ_FLAG_HIDDEN);

    load_port_baud();
    filter_reset();
    T.timer = lv_timer_create(timer_cb, 80, NULL);
    refresh(true);
    return &T;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (T.timer) lv_timer_delete(T.timer);
    T.timer = NULL;
    T.overlay = NULL;
}

static bool back(aos_app_t *self, void *inst)
{
    if (T.overlay) { sheet_close(); refresh(true); return true; }
    if (!lv_obj_has_flag(T.kb, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(T.kb, LV_OBJ_FLAG_HIDDEN); return true; }
    return false;
}

void aos_app_serial_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.serial", .name = "Terminal", .icon = AOS_SYM_SERIAL_PORT,
            .color_a = 0x2F3B45, .color_b = 0x10161B,
            .flags = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE,
            .order = 110,
        },
        .create = create,
        .destroy = destroy,
        .back = back,
    };
}
