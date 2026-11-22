/*
 * P4OS - Office: memory, strings, files, UTF-8.
 */
#include "of.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(AOS_SIM) && !defined(OF_HOST)
#include "esp_heap_caps.h"
#endif

/* -------------------------------------------------------------------------- */
/* Memory: PSRAM on the board (docs/MEMORY.md)                                 */
/* -------------------------------------------------------------------------- */

void *of_alloc(size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM) || defined(OF_HOST)
    return calloc(1, n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
#endif
}

void *of_realloc(void *p, size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM) || defined(OF_HOST)
    return realloc(p, n);
#else
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!q) q = realloc(p, n);
    return q;
#endif
}

void of_free(void *p)
{
#if defined(AOS_SIM) || defined(OF_HOST)
    free(p);
#else
    heap_caps_free(p);
#endif
}

char *of_strndup(const char *s, size_t n)
{
    char *d = of_alloc(n + 1);
    if (d && n) memcpy(d, s, n);
    return d;
}

char *of_strdup(const char *s)
{
    return s ? of_strndup(s, strlen(s)) : NULL;
}

/* -------------------------------------------------------------------------- */
/* String builder                                                              */
/* -------------------------------------------------------------------------- */

static bool sb_grow(of_sb_t *b, size_t more)
{
    if (b->oom) return false;
    if (b->n + more + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + more + 1) cap += cap / 2 + 256;
    char *s = of_realloc(b->s, cap);
    if (!s) {
        b->oom = true;
        return false;
    }
    b->s = s;
    b->cap = cap;
    return true;
}

void of_sb_put(of_sb_t *b, const char *s, size_t n)
{
    if (!sb_grow(b, n)) return;
    if (n) memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void of_sb_puts(of_sb_t *b, const char *s)
{
    of_sb_put(b, s, strlen(s));
}

void of_sb_putc(of_sb_t *b, char c)
{
    of_sb_put(b, &c, 1);
}

void of_sb_printf(of_sb_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        of_sb_put(b, tmp, (size_t)n);
        return;
    }
    if (!sb_grow(b, (size_t)n)) return;
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

void of_sb_xml(of_sb_t *b, const char *s, size_t n)
{
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        case '"': rep = "&quot;"; break;
        case '\t': case '\n': case '\r': break;
        default:
            if (c < 0x20) rep = "";         /* not allowed in XML 1.0 */
            break;
        }
        if (rep) {
            of_sb_put(b, s + from, i - from);
            of_sb_puts(b, rep);
            from = i + 1;
        }
    }
    of_sb_put(b, s + from, n - from);
}

void of_sb_free(of_sb_t *b)
{
    of_free(b->s);
    memset(b, 0, sizeof *b);
}

char *of_sb_take(of_sb_t *b)
{
    char *s = b->s;
    if (!s) s = of_strdup("");
    memset(b, 0, sizeof *b);
    return s;
}

/* -------------------------------------------------------------------------- */
/* Files                                                                       */
/* -------------------------------------------------------------------------- */

uint8_t *of_file_read(const char *path, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || (size_t)n > max) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = of_alloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = n ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) {
        of_free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

bool of_file_write(const char *path, const void *data, size_t len)
{
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%.300s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        struct stat st;
        ok = stat(tmp, &st) == 0 && (size_t)st.st_size == len;
    }
    if (!ok) {
        remove(tmp);
        return false;
    }
    /* FAT cannot rename over a file */
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

bool of_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

const char *of_ext(const char *path)
{
    static char ext[12];
    const char *b = of_basename(path);
    const char *d = strrchr(b, '.');
    ext[0] = 0;
    if (!d || d == b) return ext;
    size_t i = 0;
    for (d++; *d && i < sizeof ext - 1; d++) {
        char c = *d;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[i++] = c;
    }
    ext[i] = 0;
    return ext;
}

const char *of_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* -------------------------------------------------------------------------- */
/* UTF-8                                                                       */
/* -------------------------------------------------------------------------- */

int of_utf8_len(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int of_utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

uint32_t of_utf8_get(const char *s, int *len)
{
    const uint8_t *p = (const uint8_t *)s;
    int n = of_utf8_len(p[0]);
    uint32_t cp = p[0];
    if (n == 2) cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    else if (n == 3) cp = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    else if (n == 4) cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 |
                          (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
    if (len) *len = n;
    return cp;
}

bool of_utf8_valid(const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        int k = of_utf8_len(c);
        if (k == 1 || i + (size_t)k > n) return false;
        for (int j = 1; j < k; j++)
            if ((p[i + j] & 0xC0) != 0x80) return false;
        i += (size_t)k;
    }
    return true;
}

/* Windows-1252's 0x80-0x9F, the rest is Latin-1 */
static const uint16_t CP1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
    0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };

char *of_latin1_to_utf8(const char *s, size_t n, size_t *out_len)
{
    char *o = of_alloc(n * 3 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp = c >= 0x80 && c < 0xA0 ? CP1252[c - 0x80] : c;
        k += (size_t)of_utf8_put(o + k, cp);
    }
    o[k] = 0;
    if (out_len) *out_len = k;
    return o;
}

char *of_utf8_to_latin1(const char *s, size_t n, size_t *out_len, int *lost)
{
    char *o = of_alloc(n + 1);
    if (!o) return NULL;
    size_t k = 0;
    int bad = 0;
    for (size_t i = 0; i < n;) {
        int l;
        uint32_t cp = of_utf8_get(s + i, &l);
        if (i + (size_t)l > n) l = (int)(n - i);
        i += (size_t)l;
        if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) {
            o[k++] = (char)cp;
            continue;
        }
        int m = -1;
        for (int j = 0; j < 32; j++)
            if (CP1252[j] == cp) { m = j; break; }
        if (m >= 0) o[k++] = (char)(0x80 + m);
        else { o[k++] = '?'; bad++; }
    }
    o[k] = 0;
    if (out_len) *out_len = k;
    if (lost) *lost = bad;
    return o;
}

/* -------------------------------------------------------------------------- */
/* CRC-32                                                                      */
/* -------------------------------------------------------------------------- */

static uint32_t crc_tab[256];

uint32_t of_crc32(uint32_t crc, const void *data, size_t n)
{
    if (!crc_tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            crc_tab[i] = c;
        }
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
