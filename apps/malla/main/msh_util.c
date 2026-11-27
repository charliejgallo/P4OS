/*
 * Malla - small helpers shared by every part.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 */
#include "msh_util.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

void *msh_alloc(size_t n)
{
    void *p = NULL;
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!p) p = malloc(n ? n : 1);
    if (p) memset(p, 0, n);
    return p;
}

void msh_scpy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = 0;
    if (src) {
        n = strlen(src);
        if (n > cap - 1) {
            n = cap - 1;
            /* back off a continuation byte so no character is cut in half */
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
        }
        memcpy(dst, src, n);
    }
    dst[n] = 0;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int msh_b64url_encode(const uint8_t *in, int len, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int n = 1;
        if (i + 1 < len) { v |= (uint32_t)in[i + 1] << 8; n++; }
        if (i + 2 < len) { v |= in[i + 2]; n++; }
        if (o + n + 2 > cap) return -1;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (n > 1) out[o++] = B64[(v >> 6) & 63];
        if (n > 2) out[o++] = B64[v & 63];
    }
    if (o >= cap) return -1;
    out[o] = 0;
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

int msh_b64_decode(const char *in, uint8_t *out, int cap)
{
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (; *in; in++) {
        if (*in == '=' || *in == '\n' || *in == '\r' || *in == ' ') continue;
        int v = b64_val(*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

void msh_node_id(uint32_t num, char out[12])
{
    snprintf(out, 12, "!%08lx", (unsigned long)num);
}

#define DEG2RAD 0.017453292f

float msh_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    float p1 = (float)lat1 * 1e-7f * DEG2RAD, p2 = (float)lat2 * 1e-7f * DEG2RAD;
    float dp = p2 - p1;
    /* the difference of longitudes in integer first: float has 24 bits */
    float dl = (float)(lon2 - lon1) * 1e-7f * DEG2RAD;
    float a = sinf(dp / 2) * sinf(dp / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
    return 2.0f * 6371000.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

float msh_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    float p1 = (float)lat1 * 1e-7f * DEG2RAD, p2 = (float)lat2 * 1e-7f * DEG2RAD;
    float dl = (float)(lon2 - lon1) * 1e-7f * DEG2RAD;
    float y = sinf(dl) * cosf(p2);
    float x = cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl);
    float b = atan2f(y, x) / DEG2RAD;
    return b < 0 ? b + 360.0f : b;
}

uint32_t msh_random(void)
{
    uint32_t v = 0;
    while (!v) aos_hal_random(&v, sizeof v);
    return v;
}

uint32_t msh_node_rgb(uint32_t num)
{
    /* the low three bytes of the number, as Meshtastic's apps do, brightened
     * so a dark one still reads on black */
    uint32_t r = (num >> 16) & 0xFF, g = (num >> 8) & 0xFF, b = num & 0xFF;
    uint32_t m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m < 120) {
        uint32_t add = 120 - m;
        r += add; g += add; b += add;
    }
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

uint32_t msh_now_s(void)
{
    return (uint32_t)(aos_hal_uptime_ms() / 1000u);
}

uint32_t msh_epoch(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    time_t t = time(NULL);
    return t > 1600000000 ? (uint32_t)t : 0;
}
