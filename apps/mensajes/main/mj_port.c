/*
 * P4OS - Mensajes: memory, the log, random bytes and SHA-256, for the app
 * and for the tests on the computer (mj_port.h).
 */
#include "mj_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MJ_HOST
#include <time.h>
#else
#include "aos_hal.h"
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#define MJ_ON_BOARD 1
#endif
#endif

/* ---- memory, counted ---- */

/* Every block carries its size in front (8 bytes, to keep the alignment),
 * so the count is exact and free() needs nothing else. */
static volatile size_t s_used, s_peak;
#ifndef MJ_HOST
static void *s_mem_lock;
#endif

static void count(long d)
{
#ifndef MJ_HOST
    if (!s_mem_lock) s_mem_lock = aos_hal_mutex_create();
    if (s_mem_lock) aos_hal_mutex_lock(s_mem_lock);
#endif
    s_used += (size_t)d;
    if (s_used > s_peak) s_peak = s_used;
#ifndef MJ_HOST
    if (s_mem_lock) aos_hal_mutex_unlock(s_mem_lock);
#endif
}

static void *raw_alloc(size_t n)
{
#ifdef MJ_ON_BOARD
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

static void *raw_realloc(void *p, size_t n)
{
#ifdef MJ_ON_BOARD
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (q) return q;
#endif
    return realloc(p, n);
}

void *mj_malloc(size_t n)
{
    size_t *p = (size_t *)raw_alloc(n + 8);
    if (!p) return NULL;
    p[0] = n;
    count((long)n);
    return (uint8_t *)p + 8;
}

void *mj_calloc(size_t n)
{
    void *p = mj_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *mj_realloc(void *ptr, size_t n)
{
    if (!ptr) return mj_malloc(n);
    size_t *p = (size_t *)((uint8_t *)ptr - 8);
    size_t old = p[0];
    size_t *q = (size_t *)raw_realloc(p, n + 8);
    if (!q) return NULL;
    q[0] = n;
    count((long)n - (long)old);
    return (uint8_t *)q + 8;
}

void mj_free(void *ptr)
{
    if (!ptr) return;
    size_t *p = (size_t *)((uint8_t *)ptr - 8);
    count(-(long)p[0]);
    free(p);
}

char *mj_strdup(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *d = (char *)mj_malloc(n + 1);
    if (d) memcpy(d, s ? s : "", n + 1);
    return d;
}

size_t mj_mem_used(void) { return s_used; }
size_t mj_mem_peak(void) { return s_peak; }

/* ---- log, random, clocks ---- */

void mj_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef MJ_HOST
    fprintf(stderr, "%s\n", line);
#else
    aos_hal_log("mensajes", "%s", line);
#endif
}

void mj_random(void *buf, size_t n)
{
#ifdef MJ_HOST
    arc4random_buf(buf, n);
#else
    aos_hal_random(buf, n);
#endif
}

uint32_t mj_now_ms(void)
{
#ifdef MJ_HOST
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#else
    return (uint32_t)aos_hal_uptime_ms();
#endif
}

int64_t mj_unixtime(void)
{
#ifdef MJ_HOST
    return (int64_t)time(NULL);
#else
    if (!aos_hal_time_is_valid()) return 0;
    return (int64_t)time(NULL);
#endif
}

/* ---- SHA-256 ---- */

#ifdef MJ_HOST
/* the tests' own: on the board it is the HAL's (the P4's SHA engine) */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha256_t;

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void s256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = RR(w[i - 15], 7) ^ RR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = RR(w[i - 2], 17) ^ RR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = RR(e, 6) ^ RR(e, 11) ^ RR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = RR(a, 2) ^ RR(a, 13) ^ RR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

static void s256_init(sha256_t *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void s256_update(sha256_t *c, const void *d, size_t n)
{
    const uint8_t *p = (const uint8_t *)d;
    c->len += n;
    while (n) {
        size_t k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k;
        p += k;
        n -= k;
        if (c->n == 64) {
            s256_block(c->h, c->buf);
            c->n = 0;
        }
    }
}

static void s256_final(sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    s256_update(c, &pad, 1);
    while (c->n != 56) s256_update(c, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    s256_update(c, l, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void mj_sha256(const void *d, size_t n, uint8_t out[32])
{
    sha256_t c;
    s256_init(&c);
    s256_update(&c, d, n);
    s256_final(&c, out);
}

void mj_sha256_2(const void *a, size_t an, const void *b, size_t bn, uint8_t out[32])
{
    sha256_t c;
    s256_init(&c);
    s256_update(&c, a, an);
    s256_update(&c, b, bn);
    s256_final(&c, out);
}
#else
void mj_sha256(const void *d, size_t n, uint8_t out[32])
{
    aos_hal_sha256(d, n, out);
}

void mj_sha256_2(const void *a, size_t an, const void *b, size_t bn, uint8_t out[32])
{
    aos_hal_sha256_ctx_t *c = aos_hal_sha256_begin();
    if (!c) {
        /* no memory for a context: the two pieces copied together */
        uint8_t *t = (uint8_t *)mj_malloc(an + bn);
        if (!t) {
            memset(out, 0, 32);
            return;
        }
        memcpy(t, a, an);
        memcpy(t + an, b, bn);
        aos_hal_sha256(t, an + bn, out);
        mj_free(t);
        return;
    }
    aos_hal_sha256_update(c, a, an);
    aos_hal_sha256_update(c, b, bn);
    aos_hal_sha256_end(c, out);
}
#endif
