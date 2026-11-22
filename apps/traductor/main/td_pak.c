/*
 * TRADUCTOR - the packs (see td_pak.h)
 */
#include "td_pak.h"

#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(TD_BENCH)
#include "esp_heap_caps.h"
#define TD_ON_BOARD 1
#endif

#define ENTRY_SIZE 48

static size_t s_used;

void *td_malloc(size_t n)
{
    size_t total = n + 16;
#ifdef TD_ON_BOARD
    /* PSRAM or nothing: internal RAM is the system's */
    uint32_t *b = (uint32_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint32_t *b = (uint32_t *)malloc(total);
#endif
    if (!b) return NULL;
    b[0] = (uint32_t)n;
    b[1] = 0x7A7A7A7Au;
    s_used += n;
    return (uint8_t *)b + 16;
}

void td_free(void *p)
{
    if (!p) return;
    uint32_t *b = (uint32_t *)((uint8_t *)p - 16);
    if (b[1] == 0x7A7A7A7Au) {
        s_used -= b[0];
        b[1] = 0;
    }
    free(b);
}

size_t td_mem_used(void)
{
    return s_used;
}

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool td_pak_read(td_pak_t *p, uint32_t off, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    int i = 0;
    while (len) {
        while (i < p->parts && off >= p->part_len[i]) {
            off -= p->part_len[i];
            i++;
        }
        if (i >= p->parts) return false;
        uint32_t n = p->part_len[i] - off;
        if (n > len) n = len;
        if (fseek(p->fp[i], (long)off, SEEK_SET) != 0) return false;
        if (fread(d, 1, n, p->fp[i]) != n) return false;
        d += n;
        len -= n;
        off += n;
    }
    return true;
}

bool td_pak_open(td_pak_t *p, const char *path)
{
    memset(p, 0, sizeof *p);
    char name[200];
    for (int i = 0; i < TD_PARTS; i++) {
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
    if (!td_pak_read(p, 0, h, 8) || memcmp(h, "TRPK", 4) || rd16(h + 4) != 1) goto fail;
    p->n = rd16(h + 6);
    if (p->n <= 0) goto fail;
    uint8_t *raw = (uint8_t *)malloc((size_t)p->n * ENTRY_SIZE);
    p->e = (td_entry_t *)calloc((size_t)p->n, sizeof(td_entry_t));
    if (!raw || !p->e || !td_pak_read(p, 8, raw, (uint32_t)p->n * ENTRY_SIZE)) {
        free(raw);
        goto fail;
    }
    for (int i = 0; i < p->n; i++) {
        const uint8_t *b = raw + i * ENTRY_SIZE;
        td_entry_t *e = &p->e[i];
        memcpy(e->name, b, TD_NAME_LEN);
        e->name[TD_NAME_LEN - 1] = 0;
        e->type = rd32(b + 24);
        e->off = rd32(b + 28);
        e->len = rd32(b + 32);
        e->rows = rd16(b + 36);
        e->cols = rd16(b + 38);
        uint32_t s = rd32(b + 40), sa = rd32(b + 44);
        memcpy(&e->scale, &s, 4);
        memcpy(&e->scale_a, &sa, 4);
    }
    free(raw);
    return true;
fail:
    td_pak_close(p);
    return false;
}

void td_pak_close(td_pak_t *p)
{
    for (int i = 0; i < p->parts; i++)
        if (p->fp[i]) fclose(p->fp[i]);
    free(p->e);
    memset(p, 0, sizeof *p);
}

const td_entry_t *td_pak_find(const td_pak_t *p, const char *name)
{
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->e[i].name, name)) return &p->e[i];
    return NULL;
}

void *td_pak_load(td_pak_t *p, const td_entry_t *e)
{
    if (!e) return NULL;
    void *b = td_malloc(e->len ? e->len : 1);
    if (!b) return NULL;
    if (!td_pak_read(p, e->off, b, e->len)) {
        td_free(b);
        return NULL;
    }
    return b;
}
