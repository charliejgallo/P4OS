/*
 * Tortuga - the packs (see tg_pak.h); Bastion's, renamed
 */
#include "tg_pak.h"

#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define ENTRY_SIZE 52

/* every block carries its size in front, so the app can say how much it
 * holds (the log's line, and the check before a world is loaded) */
static size_t s_used;

void *tg_alloc(size_t n)
{
    size_t total = n + 16;
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    /* PSRAM or nothing: a malloc() that falls back to internal RAM takes
     * what the system's DMA lives on (Pinball, 0.17.0: the board panicked) */
    uint32_t *b = (uint32_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint32_t *b = (uint32_t *)malloc(total);
#endif
    if (!b) return NULL;
    b[0] = (uint32_t)n;
    b[1] = 0xB5B5B5B5u;
    s_used += n;
    return (uint8_t *)b + 16;
}

void *tg_malloc(size_t n)
{
    void *p = tg_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void tg_free(void *p)
{
    if (!p) return;
    uint32_t *b = (uint32_t *)((uint8_t *)p - 16);
    if (b[1] == 0xB5B5B5B5u) {
        s_used -= b[0];
        b[1] = 0;
    }
    free(b);
}

size_t tg_mem_used(void)
{
    return s_used;
}

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

bool tg_pak_open(tg_pak_t *p, const char *path)
{
    memset(p, 0, sizeof *p);
    char name[200];
    for (int i = 0; i < TGX_PARTS; i++) {
        if (i == 0) snprintf(name, sizeof name, "%.180s", path);
        else snprintf(name, sizeof name, "%.180s.%d", path, i);
        FILE *f = fopen(name, "rb");
        if (!f) break;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        p->fp[i] = f;
        p->part_len[i] = len > 0 ? (uint32_t)len : 0;
        p->parts++;
    }
    if (!p->parts) return false;
    uint8_t h[8];
    if (fread(h, 1, 8, p->fp[0]) != 8 || memcmp(h, "BSPK", 4) || rd16(h + 4) != 1) goto fail;
    p->n = rd16(h + 6);
    if (p->n <= 0 || p->n > 4096) goto fail;
    uint8_t *tab = (uint8_t *)tg_alloc((size_t)p->n * ENTRY_SIZE);
    p->e = (tg_entry_t *)tg_malloc((size_t)p->n * sizeof(tg_entry_t));
    if (!tab || !p->e || fread(tab, ENTRY_SIZE, (size_t)p->n, p->fp[0]) != (size_t)p->n) {
        tg_free(tab);
        goto fail;
    }
    for (int i = 0; i < p->n; i++) {
        const uint8_t *b = tab + i * ENTRY_SIZE;
        tg_entry_t *e = &p->e[i];
        memcpy(e->name, b, TGX_NAME_LEN);
        e->name[TGX_NAME_LEN - 1] = 0;
        e->type = b[24];
        e->flags = b[25];
        e->w = rd16(b + 26);
        e->h = rd16(b + 28);
        e->x = (int16_t)rd16(b + 30);
        e->y = (int16_t)rd16(b + 32);
        e->off = rd32(b + 34);
        e->clen = rd32(b + 38);
        e->rawlen = rd32(b + 42);
        e->extra = rd32(b + 46);
    }
    tg_free(tab);
    return true;
fail:
    tg_pak_close(p);
    return false;
}

void tg_pak_close(tg_pak_t *p)
{
    for (int i = 0; i < TGX_PARTS; i++) {
        if (p->fp[i]) fclose(p->fp[i]);
    }
    tg_free(p->e);
    memset(p, 0, sizeof *p);
}

const tg_entry_t *tg_pak_find(const tg_pak_t *p, const char *name)
{
    for (int i = 0; i < p->n; i++) {
        if (!strcmp(p->e[i].name, name)) return &p->e[i];
    }
    return NULL;
}

/* reads across the parts as if they were one file */
static bool read_at(tg_pak_t *p, uint32_t off, uint8_t *dst, uint32_t len)
{
    for (int i = 0; i < p->parts && len; i++) {
        if (off >= p->part_len[i]) {
            off -= p->part_len[i];
            continue;
        }
        uint32_t n = p->part_len[i] - off;
        if (n > len) n = len;
        if (fseek(p->fp[i], (long)off, SEEK_SET) != 0) return false;
        if (fread(dst, 1, n, p->fp[i]) != n) return false;
        dst += n;
        len -= n;
        off = 0;
    }
    return len == 0;
}

static bool lz4_decode(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen)
{
    size_t i = 0, o = 0;
    while (i < slen) {
        uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= slen) return false;
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (i + lit > slen || o + lit > dlen) return false;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= slen) break;
        if (i + 2 > slen) return false;
        size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        size_t m = (size_t)(tok & 15) + 4;
        if ((tok & 15) == 15) {
            uint8_t b;
            do {
                if (i >= slen) return false;
                b = src[i++];
                m += b;
            } while (b == 255);
        }
        if (off == 0 || off > o || o + m > dlen) return false;
        uint8_t *d = dst + o;
        const uint8_t *s = d - off;
        if (off >= m) {
            memcpy(d, s, m);
        } else {
            for (size_t k = 0; k < m; k++) d[k] = s[k];
        }
        o += m;
    }
    return o == dlen;
}

void *tg_pak_load(tg_pak_t *p, const tg_entry_t *e)
{
    if (!p->parts || !e) return NULL;
    uint8_t *raw = (uint8_t *)tg_alloc(e->rawlen + 2);
    if (!raw) {
        p->fails++;
        return NULL;
    }
    raw[e->rawlen] = 0;
    raw[e->rawlen + 1] = 0;
    if (e->clen == e->rawlen) {
        if (!read_at(p, e->off, raw, e->rawlen)) goto fail;
        return raw;
    }
    uint8_t *src = (uint8_t *)tg_alloc(e->clen);
    if (!src) goto fail;
    bool ok = read_at(p, e->off, src, e->clen) && lz4_decode(src, e->clen, raw, e->rawlen);
    tg_free(src);
    if (ok) return raw;
fail:
    tg_free(raw);
    p->fails++;
    return NULL;
}

void *tg_pak_get(tg_pak_t *p, const char *name, const tg_entry_t **out)
{
    const tg_entry_t *e = tg_pak_find(p, name);
    if (out) *out = e;
    return e ? tg_pak_load(p, e) : NULL;
}
