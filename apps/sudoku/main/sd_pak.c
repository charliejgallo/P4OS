/*
 * SUDOKU - the pack (see sd_pak.h). The reader is Bastion's, cut down to
 * one file.
 */
#include "sd_pak.h"

#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define ENTRY_SIZE 52

void *sd_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    /* PSRAM or nothing: internal RAM is the system's (docs/MEMORY.md) */
    return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n ? n : 1);
#endif
}

void sd_free(void *p)
{
    free(p);
}

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool sd_pak_open(sd_pak_t *p, const char *path)
{
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%.150s", path);
    p->fp = fopen(path, "rb");
    if (!p->fp) return false;
    uint8_t h[8];
    if (fread(h, 1, 8, p->fp) != 8 || memcmp(h, "SDPK", 4) || rd16(h + 4) != 1) goto fail;
    p->n = rd16(h + 6);
    if (p->n <= 0 || p->n > 4096) goto fail;
    uint8_t *tab = (uint8_t *)sd_alloc((size_t)p->n * ENTRY_SIZE);
    p->e = (sd_pak_entry_t *)sd_alloc((size_t)p->n * sizeof(sd_pak_entry_t));
    if (!tab || !p->e || fread(tab, ENTRY_SIZE, (size_t)p->n, p->fp) != (size_t)p->n) {
        sd_free(tab);
        goto fail;
    }
    for (int i = 0; i < p->n; i++) {
        const uint8_t *b = tab + i * ENTRY_SIZE;
        sd_pak_entry_t *e = &p->e[i];
        memcpy(e->name, b, SDPK_NAME_LEN);
        e->name[SDPK_NAME_LEN - 1] = 0;
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
    sd_free(tab);
    return true;
fail:
    sd_pak_close(p);
    return false;
}

void sd_pak_close(sd_pak_t *p)
{
    if (p->fp) fclose(p->fp);
    sd_free(p->e);
    memset(p, 0, sizeof *p);
}

const sd_pak_entry_t *sd_pak_find(const sd_pak_t *p, const char *name)
{
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].name, name)) return &p->e[i];
    return NULL;
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

void *sd_pak_load(sd_pak_t *p, const sd_pak_entry_t *e)
{
    if (!p->fp || !e) return NULL;
    uint8_t *raw = (uint8_t *)sd_alloc(e->rawlen + 2);
    if (!raw) return NULL;
    raw[e->rawlen] = 0;
    raw[e->rawlen + 1] = 0;
    if (fseek(p->fp, (long)e->off, SEEK_SET) != 0) goto fail;
    if (e->clen == e->rawlen) {
        if (fread(raw, 1, e->rawlen, p->fp) != e->rawlen) goto fail;
        return raw;
    }
    uint8_t *src = (uint8_t *)sd_alloc(e->clen);
    if (!src) goto fail;
    bool ok = fread(src, 1, e->clen, p->fp) == e->clen && lz4_decode(src, e->clen, raw, e->rawlen);
    sd_free(src);
    if (ok) return raw;
fail:
    sd_free(raw);
    return NULL;
}
