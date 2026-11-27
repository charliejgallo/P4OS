/*
 * P4OS - Vision: codes in the picture.
 *
 * QR: quirc (main/quirc, ISC, Daniel Beer), on the frame in grey.
 *
 * Bar codes, EAN-13 (and UPC-A, which is an EAN-13 starting with 0) and
 * EAN-8, read here: rows across the middle of the frame, each turned black
 * and white against its own moving average, cut into runs, and every place
 * a guard (bar, space, bar of one module) starts is tried in both
 * directions. A digit is four runs that add up to seven modules; it is the
 * pattern of the table nearest to the runs scaled to seven. A code counts
 * when its check digit is right and two rows agree.
 */
#include "vision.h"
#include "quirc/quirc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* quirc's identify.c, in single precision, takes rint from here: to nearest,
 * ties to even (the firmware exports roundf, not rintf) */
float vi_rintf(float x)
{
    float r = roundf(x);
    if (fabsf(r - x) == 0.5f) r = 2.f * roundf(x * 0.5f);
    return r;
}

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
/* quirc's assert()s (the app is built without NDEBUG): the firmware does
 * not export newlib's, so they land here */
void __assert_func(const char *file, int line, const char *func, const char *expr);
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    aos_hal_log("vision", "assert %s at %s:%d (%s)", expr ? expr : "?", file ? file : "?", line, func ? func : "?");
    abort();
}
#endif

static struct quirc *s_q;
static int s_w, s_h;
static uint8_t *s_row;
static int *s_runs;

bool vi_codes_init(int w, int h)
{
    if (s_q && s_w == w && s_h == h) return true;
    vi_codes_free();
    s_q = quirc_new();
    if (!s_q || quirc_resize(s_q, w, h) < 0) {
        vi_codes_free();
        return false;
    }
    s_row = (uint8_t *)malloc((size_t)w);
    s_runs = (int *)malloc(sizeof(int) * (size_t)w);
    if (!s_row || !s_runs) {
        vi_codes_free();
        return false;
    }
    s_w = w;
    s_h = h;
    return true;
}

void vi_codes_free(void)
{
    if (s_q) quirc_destroy(s_q);
    s_q = NULL;
    free(s_row);
    free(s_runs);
    s_row = NULL;
    s_runs = NULL;
    s_w = s_h = 0;
}

static inline uint8_t luma(uint16_t p)
{
    int r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

/* ---- EAN / UPC --------------------------------------------------------------- */

/* the L code of each digit, as the widths of its space, bar, space, bar */
static const uint8_t L_W[10][4] = {{3, 2, 1, 1}, {2, 2, 2, 1}, {2, 1, 2, 2}, {1, 4, 1, 1}, {1, 1, 3, 2},
                                   {1, 2, 3, 1}, {1, 1, 1, 4}, {1, 3, 1, 2}, {1, 2, 1, 3}, {3, 1, 1, 2}};
/* EAN-13's first digit, from which of the left six are G codes (bit 5: the first) */
static const uint8_t FIRST_PARITY[10] = {0x00, 0x0B, 0x0D, 0x0E, 0x13, 0x19, 0x1C, 0x15, 0x16, 0x1A};

/* the digit four runs make; *g: it is a G code (the L code backwards). -1: none */
static int match_digit(const int *r, bool allow_g, bool *g)
{
    int total = r[0] + r[1] + r[2] + r[3];
    if (total <= 0) return -1;
    int best = -1, best_err = 1 << 30;
    bool best_g = false;
    for (int d = 0; d < 10; d++) {
        for (int k = 0; k < (allow_g ? 2 : 1); k++) {
            int err = 0;
            for (int i = 0; i < 4; i++) {
                int want = k ? L_W[d][3 - i] : L_W[d][i];
                int e = r[i] * 7 * 16 / total - want * 16;
                err += e < 0 ? -e : e;
            }
            if (err < best_err) {
                best_err = err;
                best = d;
                best_g = k;
            }
        }
    }
    /* a quarter of a module per run on average, at most */
    if (best_err > 4 * 16 / 2) return -1;
    *g = best_g;
    return best;
}

static bool guard_ok(const int *r, int n, float m)
{
    for (int i = 0; i < n; i++)
        if (r[i] < m * 0.5f || r[i] > m * 1.7f) return false;
    return true;
}

static bool check_ok(const char *d, int n)
{
    int sum = 0;
    for (int i = 0; i < n - 1; i++) {
        int v = d[i] - '0';
        /* weights 3 and 1 from the right, the check digit apart */
        sum += ((n - 1 - i) % 2) ? v * 3 : v;
    }
    return (10 - sum % 10) % 10 == d[n - 1] - '0';
}

/* An EAN-13 from run 'at' (a bar) of 'r'; digits into out (14 bytes) */
static bool try_ean13(const int *r, int nr, int at, char *out)
{
    if (at + 59 > nr) return false;
    const int *p = r + at;
    float m = (p[0] + p[1] + p[2]) / 3.f;
    if (!guard_ok(p, 3, m)) return false;
    char d[14];
    int parity = 0;
    for (int i = 0; i < 6; i++) {
        bool g;
        int v = match_digit(p + 3 + 4 * i, true, &g);
        if (v < 0) return false;
        d[1 + i] = (char)('0' + v);
        parity = (parity << 1) | (g ? 1 : 0);
    }
    if (!guard_ok(p + 27, 5, m)) return false;
    for (int i = 0; i < 6; i++) {
        bool g;
        int v = match_digit(p + 32 + 4 * i, false, &g);
        if (v < 0) return false;
        d[7 + i] = (char)('0' + v);
    }
    if (!guard_ok(p + 56, 3, m)) return false;
    int first = -1;
    for (int k = 0; k < 10; k++)
        if (FIRST_PARITY[k] == parity) first = k;
    if (first < 0) return false;
    d[0] = (char)('0' + first);
    d[13] = 0;
    if (!check_ok(d, 13)) return false;
    memcpy(out, d, 14);
    return true;
}

static bool try_ean8(const int *r, int nr, int at, char *out)
{
    if (at + 43 > nr) return false;
    const int *p = r + at;
    float m = (p[0] + p[1] + p[2]) / 3.f;
    if (!guard_ok(p, 3, m)) return false;
    char d[9];
    for (int i = 0; i < 4; i++) {
        bool g;
        int v = match_digit(p + 3 + 4 * i, false, &g);
        if (v < 0) return false;
        d[i] = (char)('0' + v);
    }
    if (!guard_ok(p + 19, 5, m)) return false;
    for (int i = 0; i < 4; i++) {
        bool g;
        int v = match_digit(p + 24 + 4 * i, false, &g);
        if (v < 0) return false;
        d[4 + i] = (char)('0' + v);
    }
    if (!guard_ok(p + 40, 3, m)) return false;
    d[8] = 0;
    if (!check_ok(d, 8)) return false;
    memcpy(out, d, 9);
    return true;
}

/* one row: its runs, starting with a bar; the codes found, into found[] */
typedef struct {
    char digits[14];
    vi_code_kind_t kind;
    int y, x0, x1, hits;
} bar_hit_t;

static void scan_row(const uint8_t *l, int w, int y, bar_hit_t *hits, int *nhits, int max)
{
    /* black and white against a moving average of 2*R+1 pixels */
    const int R = 16;
    int sum = 0;
    for (int i = 0; i < R && i < w; i++) sum += l[i];
    int nr = 0, run = 0, first_x = -1;
    bool prev = false;
    static int xs[1024];
    for (int x = 0; x < w; x++) {
        if (x + R < w) sum += l[x + R];
        if (x - R - 1 >= 0) sum -= l[x - R - 1];
        int lo = x - R < 0 ? 0 : x - R, hi = x + R >= w ? w - 1 : x + R;
        bool bar = l[x] * (hi - lo + 1) < sum - 8 * (hi - lo + 1);
        if (first_x < 0) {
            if (!bar) continue;
            first_x = x;
            prev = true;
            run = 0;
        }
        if (bar != prev) {
            if (nr < 1024) xs[nr] = x - run;
            s_runs[nr++] = run;
            run = 0;
            prev = bar;
        }
        run++;
    }
    if (run && first_x >= 0) {
        if (nr < 1024) xs[nr] = w - run;
        s_runs[nr++] = run;
    }
    /* runs at even indexes are bars; try every bar, both ways */
    for (int dir = 0; dir < 2; dir++) {
        static int rr[1024];
        int n = nr < 1024 ? nr : 1024;
        for (int i = 0; i < n; i++) rr[i] = dir ? s_runs[n - 1 - i] : s_runs[i];
        int first_bar = dir ? ((n - 1) % 2 == 0 ? 0 : 1) : 0;
        for (int at = first_bar; at < n; at += 2) {
            char d[14];
            vi_code_kind_t kind;
            int len;
            if (try_ean13(rr, n, at, d)) {
                kind = d[0] == '0' ? VI_CODE_UPCA : VI_CODE_EAN13;
                len = 59;
            } else if (try_ean8(rr, n, at, d)) {
                kind = VI_CODE_EAN8;
                len = 43;
            } else {
                continue;
            }
            int a = dir ? n - 1 - (at + len - 1) : at, b = dir ? n - 1 - at : at + len - 1;
            int x0 = xs[a], x1 = xs[b] + s_runs[b];
            int k;
            for (k = 0; k < *nhits; k++)
                if (strcmp(hits[k].digits, d) == 0) break;
            if (k < *nhits) {
                hits[k].hits++;
                if (y > hits[k].y) hits[k].y = y;
            } else if (*nhits < max) {
                bar_hit_t *h = &hits[(*nhits)++];
                memcpy(h->digits, d, sizeof h->digits);
                h->kind = kind;
                h->y = y;
                h->x0 = x0;
                h->x1 = x1;
                h->hits = 1;
            }
            at += len - 1;
        }
    }
}

int vi_codes_scan(const uint16_t *px, int w, int h, vi_code_t *out, int max)
{
    if (!vi_codes_init(w, h)) return 0;
    int n = 0;

    /* QR */
    int qw, qh;
    uint8_t *g = quirc_begin(s_q, &qw, &qh);
    for (int i = 0; i < w * h; i++) g[i] = luma(px[i]);
    quirc_end(s_q);
    int nq = quirc_count(s_q);
    for (int i = 0; i < nq && n < max; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(s_q, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            /* a mirrored code (a selfie camera) */
            quirc_flip(&code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        }
        vi_code_t *c = &out[n++];
        c->kind = VI_CODE_QR;
        int x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (int k = 0; k < 4; k++) {
            if (code.corners[k].x < x0) x0 = code.corners[k].x;
            if (code.corners[k].y < y0) y0 = code.corners[k].y;
            if (code.corners[k].x > x1) x1 = code.corners[k].x;
            if (code.corners[k].y > y1) y1 = code.corners[k].y;
        }
        c->x0 = (int16_t)x0;
        c->y0 = (int16_t)y0;
        c->x1 = (int16_t)x1;
        c->y1 = (int16_t)y1;
        int len = data.payload_len < VI_CODE_LEN - 1 ? data.payload_len : VI_CODE_LEN - 1;
        memcpy(c->text, data.payload, (size_t)len);
        c->text[len] = 0;
    }

    /* bars: every 6th row across the middle two thirds (quirc thresholded
     * its copy in place, so the rows are made again) */
    bar_hit_t hits[8];
    int nh = 0;
    for (int y = h / 6; y < h * 5 / 6; y += 6) {
        const uint16_t *src = px + (size_t)y * w;
        for (int x = 0; x < w; x++) s_row[x] = luma(src[x]);
        scan_row(s_row, w, y, hits, &nh, 8);
    }
    for (int k = 0; k < nh && n < max; k++) {
        if (hits[k].hits < 2) continue;
        vi_code_t *c = &out[n++];
        c->kind = hits[k].kind;
        c->x0 = (int16_t)hits[k].x0;
        c->x1 = (int16_t)hits[k].x1;
        c->y0 = (int16_t)(hits[k].y - 6 * hits[k].hits);
        c->y1 = (int16_t)hits[k].y;
        if (c->y0 < 0) c->y0 = 0;
        snprintf(c->text, VI_CODE_LEN, "%s", hits[k].kind == VI_CODE_UPCA ? hits[k].digits + 1 : hits[k].digits);
    }
    return n;
}
