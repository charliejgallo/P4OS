/*
 * P4OS - Monitor serie: the portal's page (web/plotter.js).
 *
 * Through aos_hal_live_* (docs/PORTAL-PAGES.md, "Live data"), as the RF app
 * does, and only while a page asked something in the last five seconds:
 *
 *   data    4 times a second, binary little-endian float32: ns, n, the
 *           window in seconds, paused, then n rows of the time (seconds
 *           back from now, <= 0) and a value per series (NaN where none).
 *           The chart's own columns, 600 across.
 *   state   once a second, JSON: the connection, the counts, the series
 *           with their colour, last value, the window's low, high and
 *           average, the alarms; and the last lines with what came out.
 *
 * The page sends lines of key=value: pause=0|1, win=<s>, clear, rec=0|1,
 * send=<text> (the line ending of the app's Enviar sheet is added).
 */
#include "pl.h"

#define LIVE_COLS 600

static pl_cols_t s_cols;
static uint32_t s_data_at, s_state_at;
static float *s_buf;
static char *s_json;
#define JSON_MAX 12000

void pl_live_stop(void)
{
    aos_hal_live_clear(PL_ID);
    pl_cols_free(&s_cols);
    free(s_buf);
    free(s_json);
    s_buf = NULL;
    s_json = NULL;
}

static void put_data(void)
{
    if (!s_buf) s_buf = pl_ps_alloc(sizeof(float) * (4 + LIVE_COLS * (1 + PL_MAXS)));
    if (!s_buf || !pl_cols_alloc(&s_cols, LIVE_COLS)) return;
    uint32_t now = pl_now();
    uint32_t t1 = PLA.paused ? PLA.end_t : now;
    uint32_t t0 = t1 > PLA.win_ms ? t1 - PLA.win_ms : 0;
    pl_data_columns(&s_cols, t0, t1, LIVE_COLS);
    int ns = PLD.ns, n = 0;
    float *o = s_buf + 4;
    for (int k = 0; k < s_cols.ncols; k++) {
        bool any = false;
        uint32_t t = 0;
        for (int s = 0; s < ns; s++)
            if (!PL_ISNAN(s_cols.last[s * s_cols.cap + k])) { any = true; t = s_cols.lt[s * s_cols.cap + k]; }
        if (!any) continue;
        *o++ = -(float)(now - t) / 1000.0f;
        for (int s = 0; s < ns; s++) *o++ = s_cols.last[s * s_cols.cap + k];
        n++;
    }
    s_buf[0] = (float)ns;
    s_buf[1] = (float)n;
    s_buf[2] = (float)PLA.win_ms / 1000.0f;
    s_buf[3] = PLA.paused ? (float)(now - t1) / 1000.0f : 0;
    aos_hal_live_put(PL_ID, "data", "application/octet-stream", s_buf, sizeof(float) * (size_t)(4 + n * (1 + ns)));
}

/* a JSON string, escaped */
static size_t jstr(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    if (k < cap) o[k++] = '"';
    for (; *s && k + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(o + k, cap - k, "\\u%04x", c);
        else o[k++] = (char)c;
    }
    if (k < cap) o[k++] = '"';
    if (k < cap) o[k] = 0;
    return k;
}

static size_t jnum(char *o, size_t cap, float v)
{
    if (PL_ISNAN(v) || fabsf(v) > 3.4e38f) return (size_t)snprintf(o, cap, "null");
    return (size_t)snprintf(o, cap, "%.7g", (double)v);
}

#define ADD(...) do { if (k < JSON_MAX) k += (size_t)snprintf(s_json + k, JSON_MAX - k, __VA_ARGS__); } while (0)
#define ADDS(str) do { if (k < JSON_MAX) k += jstr(s_json + k, JSON_MAX - k, str); } while (0)
#define ADDN(v) do { if (k < JSON_MAX) k += jnum(s_json + k, JSON_MAX - k, v); } while (0)

static void put_state(void)
{
    if (!s_json) s_json = pl_ps_alloc(JSON_MAX);
    if (!s_json) return;
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    size_t k = 0;
    ADD("{\"ch\":\"%c\",\"running\":%s,\"port\":", PLS.ch ? 'B' : 'A', st.running ? "true" : "false");
    ADDS(st.running ? st.port : "");
    ADD(",\"baud\":%u,\"frame\":", (unsigned)(st.running ? st.baud : PLA.baud));
    ADDS(st.running ? st.frame : PLA.frame);
    ADD(",\"lines\":%u,\"used\":%u,\"lost\":%u,\"rows\":%u,\"fmt\":", (unsigned)PLA.lines, (unsigned)PLA.used,
        (unsigned)PLA.lost, (unsigned)PLD.rows);
    ADDS(pl_fmt_name());
    ADD(",\"paused\":%s,\"win\":%u,\"mode\":\"%s\",\"rec\":%s,\"recpath\":", PLA.paused ? "true" : "false",
        (unsigned)(PLA.win_ms / 1000), PLS.mode == PL_MODE_XY ? "xy" : "time", pl_csv_on() ? "true" : "false");
    ADDS(pl_csv_on() ? pl_csv_path() : "");
    ADD(",\"big\":%d,\"series\":[", PLS.big);
    for (int i = 0; i < PLD.ns; i++) {
        const pl_series_t *s = &PLD.s[i];
        float mn = NAN, mx = NAN, avg = NAN, last = NAN;
        if (s_cols.ncols && s_cols.sn[i]) {
            mn = s_cols.smin[i];
            mx = s_cols.smax[i];
            avg = s_cols.ssum[i] / (float)s_cols.sn[i];
            last = s_cols.slast[i];
        }
        if (!PLA.paused && s->has) last = s->last;
        ADD("%s{\"name\":", i ? "," : "");
        ADDS(s->name);
        ADD(",\"unit\":");
        ADDS(s->unit);
        ADD(",\"color\":\"#%06X\",\"dec\":%d,\"hidden\":%s,\"last\":", (unsigned)s->col, s->dec < 0 ? 2 : s->dec,
            s->hidden ? "true" : "false");
        ADDN(last);
        ADD(",\"min\":");
        ADDN(mn);
        ADD(",\"max\":");
        ADDN(mx);
        ADD(",\"avg\":");
        ADDN(avg);
        ADD(",\"lo\":");
        ADDN(s->lo_on ? s->lo : NAN);
        ADD(",\"hi\":");
        ADDN(s->hi_on ? s->hi : NAN);
        ADD(",\"state\":%d}", s->state);
    }
    ADD("],\"raw\":[");
    uint32_t have = PLA.nraw < 10 ? PLA.nraw : 10;
    for (uint32_t i = 0; i < have; i++) {
        const pl_rawline_t *r = &PLA.raw[(PLA.nraw - have + i) % PL_RAW_N];
        ADD("%s[", i ? "," : "");
        ADDS(r->text);
        ADD(",");
        ADDS(r->got);
        ADD("]");
    }
    ADD("]}");
    if (k >= JSON_MAX) return;      /* cut: better nothing than broken JSON */
    aos_hal_live_put(PL_ID, "state", "application/json", s_json, k);
}

static void take(void)
{
    char m[300];
    for (int i = 0; i < 8; i++) {
        int n = aos_hal_live_take(PL_ID, m, sizeof m - 1);
        if (n <= 0) break;
        m[n] = 0;
        while (n && (m[n - 1] == '\n' || m[n - 1] == '\r')) m[--n] = 0;
        char *v = strchr(m, '=');
        if (v) *v++ = 0;
        else v = m + n;
        if (!strcmp(m, "pause")) {
            PLA.paused = atoi(v) != 0;
            if (PLA.paused) PLA.end_t = pl_now();
        } else if (!strcmp(m, "win")) {
            long s = strtol(v, NULL, 10);
            if (s >= 2 && s <= 36000) PLA.win_ms = (uint32_t)s * 1000;
        } else if (!strcmp(m, "clear")) {
            aos_serial_stat_t st;
            aos_serial_stat(PLS.ch, &st);
            pl_data_clear();
            PLA.next = st.lines;
        } else if (!strcmp(m, "rec")) {
            if (atoi(v) && !pl_csv_on()) pl_csv_start();
            else if (!atoi(v)) pl_csv_stop();
        } else if (!strcmp(m, "send")) {
            static const char *const EOL[4] = { "\r\n", "\n", "\r", "" };
            char b[280];
            int len = snprintf(b, sizeof b, "%.250s%s", v, EOL[PLS.eol & 3]);
            if (len > 0) aos_serial_send(PLS.ch, b, len);
        }
        pl_chart_want();
        pl_refresh(true);
    }
}

void pl_live_poll(void)
{
    take();
    if (aos_hal_live_idle_ms(PL_ID) > 5000) return;
    uint32_t now = pl_now();
    if (now - s_data_at >= 250) {
        s_data_at = now;
        put_data();
    }
    if (now - s_state_at >= 1000) {
        s_state_at = now;
        put_state();
    }
}
