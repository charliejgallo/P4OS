/*
 * P4OS - Logic: numbers for people (times, frequencies, sample counts and
 * the decoders' values).
 */
#include "la.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* three significant figures, trailing zeros dropped */
static void num3(float v, char *out, size_t n)
{
    char b[24];
    if (v >= 100) snprintf(b, sizeof b, "%.0f", (double)v);
    else if (v >= 10) snprintf(b, sizeof b, "%.1f", (double)v);
    else snprintf(b, sizeof b, "%.2f", (double)v);
    if (strchr(b, '.')) {
        size_t l = strlen(b);
        while (l && b[l - 1] == '0') b[--l] = 0;
        if (l && b[l - 1] == '.') b[--l] = 0;
    }
    snprintf(out, n, "%s", b);
}

void la_fmt_time(float s, char *out, size_t n)
{
    char b[24];
    const char *neg = s < 0 ? "-" : "";
    float a = fabsf(s);
    if (a == 0) { snprintf(out, n, "0 s"); return; }
    /* a hair under the unit is the unit: 0.999999 ms reads 1 ms */
    if (a >= 0.9995f) { num3(a, b, sizeof b); snprintf(out, n, "%s%s s", neg, b); }
    else if (a >= 0.9995e-3f) { num3(a * 1e3f, b, sizeof b); snprintf(out, n, "%s%s ms", neg, b); }
    else if (a >= 0.9995e-6f) { num3(a * 1e6f, b, sizeof b); snprintf(out, n, "%s%s µs", neg, b); }
    else { num3(a * 1e9f, b, sizeof b); snprintf(out, n, "%s%s ns", neg, b); }
}

void la_fmt_freq(float hz, char *out, size_t n)
{
    char b[24];
    if (hz >= 0.9995e6f) { num3(hz / 1e6f, b, sizeof b); snprintf(out, n, "%s MHz", b); }
    else if (hz >= 0.9995e3f) { num3(hz / 1e3f, b, sizeof b); snprintf(out, n, "%s kHz", b); }
    else { num3(hz, b, sizeof b); snprintf(out, n, "%s Hz", b); }
}

void la_fmt_rate(uint32_t hz, char *out, size_t n)
{
    if (hz >= 1000000 && hz % 1000000 == 0) snprintf(out, n, "%u MHz", (unsigned)(hz / 1000000));
    else if (hz >= 1000000) snprintf(out, n, "%.2f MHz", (double)((float)hz / 1e6f));
    else if (hz >= 1000 && hz % 1000 == 0) snprintf(out, n, "%u kHz", (unsigned)(hz / 1000));
    else snprintf(out, n, "%u Hz", (unsigned)hz);
}

void la_fmt_count(uint32_t v, char *out, size_t n)
{
    if (v >= 1000000 && v % 1000000 == 0) snprintf(out, n, "%uM", (unsigned)(v / 1000000));
    else if (v >= 1000000) snprintf(out, n, "%.1fM", (double)((float)v / 1e6f));
    else if (v >= 1000 && v % 1000 == 0) snprintf(out, n, "%uk", (unsigned)(v / 1000));
    else if (v >= 10000) snprintf(out, n, "%.1fk", (double)((float)v / 1e3f));   /* 155648: "155.6k" */
    else snprintf(out, n, "%u", (unsigned)v);
}

void la_fmt_value(uint32_t v, int bits, int fmt, char *out, size_t n)
{
    switch (fmt) {
    case LA_FMT_DEC:
        snprintf(out, n, "%u", (unsigned)v);
        break;
    case LA_FMT_ASCII:
        if (v >= 32 && v < 127) snprintf(out, n, "'%c'", (char)v);
        else if (v == '\n') snprintf(out, n, "\\n");
        else if (v == '\r') snprintf(out, n, "\\r");
        else if (v == '\t') snprintf(out, n, "\\t");
        else snprintf(out, n, "%02X", (unsigned)v);
        break;
    case LA_FMT_BIN: {
        if (bits > 16) bits = 16;
        if (bits < 1) bits = 8;
        size_t k = 0;
        for (int i = bits - 1; i >= 0 && k + 1 < n; i--) out[k++] = (v >> i) & 1 ? '1' : '0';
        out[k] = 0;
        break;
    }
    default:
        if (bits > 8) snprintf(out, n, "%03X", (unsigned)v);
        else snprintf(out, n, "%02X", (unsigned)v);
        break;
    }
}
