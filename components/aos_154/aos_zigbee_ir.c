/*
 * P4OS - the IR blaster's codes (aos_zigbee.h), on the board and in the
 * simulator alike: Tuya's code format both ways, and the codes kept by name.
 *
 * A Tuya ("Zosung") blaster's code, as it learns it and as it takes it, is
 * base64 of a FastLZ level 1 stream whose bytes are the durations, 16-bit
 * little-endian microseconds, mark first. FastLZ level 1: a byte 000LLLLL
 * is L + 1 literal bytes that follow; any other, LLLDDDDD, a copy of what
 * came out before, L + 2 bytes long (L == 7: one more byte adds to it) from
 * (D << 8 | the next byte) + 1 back. The same reading as the Infrarrojo
 * app's page (apps/infrarrojo/web/infrarrojo.js, tuya()).
 *
 * Packing is greedy: at each byte, the longest copy in the last 8 KB if it
 * is 3 bytes or more, or else a literal. An air conditioner's frame (a few
 * hundred durations, most of them the same three) packs to a fraction, and
 * the blaster takes the code 50 bytes at a time: that is what it saves.
 *
 * The saved codes are lines of zigbee/ir.txt on the card: the blaster's IEEE
 * (16 hex digits, the first byte on the air last, as everywhere), a tab,
 * the name, a tab, the code. Read on first use, kept in PSRAM, written
 * whole on each change.
 */
#include "aos_zigbee.h"
#include "aos_hal.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#define ir_malloc(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define ir_malloc(n) malloc(n)
#endif

#define TAG "zigbee"

/* ---- base64 ------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* bytes out, -1 on a bad character or no room; spaces and '=' skipped */
static int b64_dec(const char *s, uint8_t *out, int max)
{
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (; *s; s++) {
        if (*s == '=' || *s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') continue;
        int v = b64_val(*s);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static int b64_enc(const uint8_t *in, int n, char *out, size_t size)
{
    size_t need = (size_t)(n + 2) / 3 * 4 + 1;
    if (need > size) return -1;
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ---- FastLZ level 1 ----------------------------------------------------- */

static int lz_unpack(const uint8_t *in, int n, uint8_t *out, int max)
{
    int o = 0;
    for (int i = 0; i < n;) {
        uint8_t h = in[i++];
        int len = h >> 5, dist = h & 31;
        if (!len) {
            if (i + dist + 1 > n || o + dist + 1 > max) return -1;
            memcpy(out + o, in + i, (size_t)dist + 1);
            i += dist + 1;
            o += dist + 1;
            continue;
        }
        if (len == 7) {
            if (i >= n) return -1;
            len += in[i++];
        }
        len += 2;
        if (i >= n) return -1;
        dist = (dist << 8 | in[i++]) + 1;
        if (dist > o || o + len > max) return -1;
        for (int k = 0; k < len; k++, o++) out[o] = out[o - dist];
    }
    return o;
}

static bool lz_literals(const uint8_t *in, int from, int to, uint8_t *out, int *o, int max)
{
    while (from < to) {
        int c = to - from > 32 ? 32 : to - from;
        if (*o + 1 + c > max) return false;
        out[(*o)++] = (uint8_t)(c - 1);
        memcpy(out + *o, in + from, (size_t)c);
        *o += c;
        from += c;
    }
    return true;
}

static int lz_pack(const uint8_t *in, int n, uint8_t *out, int max)
{
    int o = 0, lit = 0, i = 0;
    while (i < n) {
        int best = 0, at = 0;
        for (int j = i - 1; j >= 0 && i - j <= 8192; j--) {
            int l = 0;
            while (i + l < n && l < 264 && in[j + l] == in[i + l]) l++;
            if (l > best) {
                best = l;
                at = i - j;
                if (l == 264) break;
            }
        }
        if (best < 3) {
            i++;
            continue;
        }
        if (!lz_literals(in, lit, i, out, &o, max)) return -1;
        int len = best - 2, d = at - 1;
        if (o + 3 > max) return -1;
        if (len < 7) {
            out[o++] = (uint8_t)(len << 5 | d >> 8);
        } else {
            out[o++] = (uint8_t)(7 << 5 | d >> 8);
            out[o++] = (uint8_t)(len - 7);
        }
        out[o++] = (uint8_t)d;
        i += best;
        lit = i;
    }
    return lz_literals(in, lit, n, out, &o, max) ? o : -1;
}

/* ---- the codes both ways ------------------------------------------------ */

#define RAW_MAX 4096                    /* bytes of durations: 2048 of them */

int aos_zigbee_ir_decode(const char *b64, uint32_t *out, int max)
{
    if (!b64 || !*b64) return -1;
    size_t l = strlen(b64);
    uint8_t *packed = ir_malloc(l * 3 / 4 + 4), *raw = ir_malloc(RAW_MAX);
    int n = -1;
    if (packed && raw) {
        int pn = b64_dec(b64, packed, (int)(l * 3 / 4 + 4));
        int rn = pn > 0 ? lz_unpack(packed, pn, raw, RAW_MAX) : -1;
        if (rn >= 2) {
            n = 0;
            for (int k = 0; k + 1 < rn && n < max; k += 2) out[n++] = (uint32_t)(raw[k] | raw[k + 1] << 8);
        }
    }
    free(packed);
    free(raw);
    return n;
}

int aos_zigbee_ir_encode(const uint32_t *d, int n, char *b64, size_t size)
{
    if (!d || n <= 0 || n * 2 > RAW_MAX) return -1;
    uint8_t *raw = ir_malloc(RAW_MAX), *packed = ir_malloc(RAW_MAX + RAW_MAX / 32 + 8);
    int out = -1;
    if (raw && packed) {
        for (int k = 0; k < n; k++) {
            uint32_t v = d[k] > 65535 ? 65535 : d[k];
            raw[2 * k] = (uint8_t)v;
            raw[2 * k + 1] = (uint8_t)(v >> 8);
        }
        int pn = lz_pack(raw, 2 * n, packed, RAW_MAX + RAW_MAX / 32 + 8);
        if (pn > 0) out = b64_enc(packed, pn, b64, size);
    }
    free(raw);
    free(packed);
    return out;
}

/* ---- the codes kept by name ---------------------------------------------- */

#define MAX_CODES 96

/* <card>/zigbee and its ir.txt; false without a card */
static bool file_path(char *dir, size_t dn, char *file, size_t fn)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    snprintf(dir, dn, "%s/zigbee", root);
    snprintf(file, fn, "%s/zigbee/ir.txt", root);
    return true;
}

typedef struct {
    uint8_t ieee[8];
    char    name[AOS_ZB_IR_NAME + 1];
    char   *code;
} saved_t;

static saved_t        *s_codes;     /* PSRAM, MAX_CODES */
static int             s_ncodes;
static bool            s_loaded;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static void ieee_text(const uint8_t ie[8], char out[17])
{
    for (int k = 0; k < 8; k++) snprintf(out + 2 * k, 3, "%02x", ie[7 - k]);
}

static bool ieee_parse(const char *s, uint8_t ie[8])
{
    for (int k = 0; k < 8; k++) {
        unsigned v;
        if (sscanf(s + 2 * k, "%2x", &v) != 1) return false;
        ie[7 - k] = (uint8_t)v;
    }
    return true;
}

static void load_locked(void)
{
    if (s_loaded) return;
    if (!s_codes) s_codes = ir_malloc(sizeof(saved_t) * MAX_CODES);
    if (!s_codes) return;
    char dir[96], path[112];
    if (!file_path(dir, sizeof dir, path, sizeof path)) return;     /* tried again later: the card may come */
    s_loaded = true;
    s_ncodes = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = ir_malloc(4096);
    while (line && fgets(line, 4096, f) && s_ncodes < MAX_CODES) {
        line[strcspn(line, "\r\n")] = '\0';
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || t1 - line != 16 || !t2[1]) continue;
        saved_t *c = &s_codes[s_ncodes];
        if (!ieee_parse(line, c->ieee)) continue;
        *t2 = '\0';
        snprintf(c->name, sizeof c->name, "%s", t1 + 1);
        size_t l = strlen(t2 + 1);
        c->code = ir_malloc(l + 1);
        if (!c->code) break;
        memcpy(c->code, t2 + 1, l + 1);
        s_ncodes++;
    }
    free(line);
    fclose(f);
}

static bool write_locked(void)
{
    char dir[96], path[112], tmp[120];
    if (!file_path(dir, sizeof dir, path, sizeof path)) return false;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    mkdir(dir, 0775);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < s_ncodes && ok; i++) {
        char ie[17];
        ieee_text(s_codes[i].ieee, ie);
        ok = fprintf(f, "%s\t%s\t%s\n", ie, s_codes[i].name, s_codes[i].code) > 0;
    }
    if (fclose(f) != 0) ok = false;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) aos_hal_log(TAG, "IR: the saved codes could not be written (%d)", errno);
    return ok;
}

bool aos_zigbee_ir_save(const uint8_t ieee[8], const char *name)
{
    static char code[3000];
    uint8_t who[8];
    aos_zigbee_ir_state(who, code, sizeof code);
    if (!code[0] || !name) return false;
    /* a name on one line, without the file's tab */
    char nm[AOS_ZB_IR_NAME + 1];
    int k = 0;
    for (const char *p = name; *p && k < AOS_ZB_IR_NAME; p++) nm[k++] = *p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p;
    while (k && nm[k - 1] == ' ') k--;
    nm[k] = '\0';
    if (!nm[0]) return false;
    size_t l = strlen(code);
    char *copy = ir_malloc(l + 1);
    if (!copy) return false;
    memcpy(copy, code, l + 1);
    pthread_mutex_lock(&s_mx);
    load_locked();
    int at = -1;
    for (int i = 0; i < s_ncodes; i++)
        if (!memcmp(s_codes[i].ieee, ieee, 8) && !strcmp(s_codes[i].name, nm)) at = i;
    bool ok = s_codes != NULL && s_loaded;
    if (ok && at < 0) {
        if (s_ncodes >= MAX_CODES) ok = false;
        else s_codes[at = s_ncodes++].code = NULL;
    }
    if (ok) {
        free(s_codes[at].code);
        memcpy(s_codes[at].ieee, ieee, 8);
        snprintf(s_codes[at].name, sizeof s_codes[at].name, "%s", nm);
        s_codes[at].code = copy;
        copy = NULL;
        ok = write_locked();
    }
    pthread_mutex_unlock(&s_mx);
    free(copy);
    return ok;
}

int aos_zigbee_ir_codes(aos_zigbee_ir_code_t *out, int max)
{
    pthread_mutex_lock(&s_mx);
    load_locked();
    int n = 0;
    for (; n < s_ncodes && n < max; n++) {
        memcpy(out[n].ieee, s_codes[n].ieee, 8);
        snprintf(out[n].name, sizeof out[n].name, "%s", s_codes[n].name);
    }
    pthread_mutex_unlock(&s_mx);
    return n;
}

bool aos_zigbee_ir_send_saved(int i)
{
    static char code[3000];
    uint8_t ie[8];
    pthread_mutex_lock(&s_mx);
    load_locked();
    bool ok = i >= 0 && i < s_ncodes && strlen(s_codes[i].code) < sizeof code;
    if (ok) {
        memcpy(ie, s_codes[i].ieee, 8);
        snprintf(code, sizeof code, "%s", s_codes[i].code);
    }
    pthread_mutex_unlock(&s_mx);
    return ok && aos_zigbee_ir_send(ie, code);
}

bool aos_zigbee_ir_forget(int i)
{
    pthread_mutex_lock(&s_mx);
    load_locked();
    bool ok = i >= 0 && i < s_ncodes;
    if (ok) {
        free(s_codes[i].code);
        memmove(&s_codes[i], &s_codes[i + 1], sizeof(saved_t) * (size_t)(s_ncodes - i - 1));
        s_ncodes--;
        ok = write_locked();
    }
    pthread_mutex_unlock(&s_mx);
    return ok;
}
