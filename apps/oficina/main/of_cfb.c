/*
 * P4OS - Office: Compound File Binary (OLE2), the container of the old
 * .doc, .xls and .ppt (of_cfb.h). Sectors of 512 or 4096 bytes, the FAT
 * through the header's DIFAT and its chain, the directory, the mini stream
 * and its FAT for the small streams. Every chain is walked with a step
 * limit, so a damaged file loops nowhere.
 */
#include "of_cfb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ENDOFCHAIN 0xFFFFFFFEu
#define FREESECT   0xFFFFFFFFu

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t *sector(const of_cfb_t *c, uint32_t s)
{
    size_t off = ((size_t)s + 1) * c->ssz;
    return off + c->ssz <= c->len ? c->buf + off : NULL;
}

/* a chain of the FAT, read into one buffer */
static uint8_t *chain(const of_cfb_t *c, uint32_t start, size_t want, size_t *out)
{
    size_t cap = want ? want : c->ssz * 8, n = 0;
    uint8_t *b = of_alloc(cap + 1);
    if (!b) return NULL;
    uint32_t s = start;
    for (uint32_t steps = 0; s < ENDOFCHAIN - 4 && steps < c->nfat; steps++) {
        const uint8_t *p = sector(c, s);
        if (!p) break;
        if (n + c->ssz > cap) {
            size_t nc = cap * 2 + c->ssz;
            uint8_t *nb = of_realloc(b, nc + 1);
            if (!nb) break;
            b = nb;
            cap = nc;
        }
        memcpy(b + n, p, c->ssz);
        n += c->ssz;
        if (want && n >= want) break;
        s = s < c->nfat ? c->fat[s] : ENDOFCHAIN;
    }
    if (want && n > want) n = want;
    *out = n;
    return b;
}

bool of_cfb_is(const uint8_t *buf, size_t len)
{
    return len >= 512 && !memcmp(buf, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
}

bool of_cfb_open(of_cfb_t *c, const uint8_t *buf, size_t len)
{
    memset(c, 0, sizeof *c);
    if (!of_cfb_is(buf, len)) return false;
    c->buf = buf;
    c->len = len;
    uint16_t shift = rd16(buf + 0x1E), mshift = rd16(buf + 0x20);
    if (shift != 9 && shift != 12) return false;
    c->ssz = 1u << shift;
    c->mssz = 1u << (mshift > 0 && mshift < 12 ? mshift : 6);
    c->cutoff = rd32(buf + 0x38);
    if (!c->cutoff) c->cutoff = 4096;
    uint32_t nfat_sect = rd32(buf + 0x2C);
    /* the FAT's sectors: 109 in the header, the rest in the DIFAT chain */
    uint32_t per = c->ssz / 4;
    size_t max_sect = len / c->ssz + 1;
    if (nfat_sect > max_sect) nfat_sect = (uint32_t)max_sect;
    uint32_t *fs = of_alloc(((size_t)nfat_sect + 110) * sizeof *fs);
    if (!fs) return false;
    uint32_t k = 0;
    for (int i = 0; i < 109 && k < nfat_sect; i++) {
        uint32_t s = rd32(buf + 0x4C + i * 4);
        if (s >= ENDOFCHAIN - 4) break;
        fs[k++] = s;
    }
    uint32_t ds = rd32(buf + 0x44);
    for (uint32_t steps = 0; ds < ENDOFCHAIN - 4 && k < nfat_sect && steps < max_sect; steps++) {
        const uint8_t *p = sector(c, ds);
        if (!p) break;
        for (uint32_t i = 0; i + 1 < per && k < nfat_sect; i++) {
            uint32_t s = rd32(p + i * 4);
            if (s < ENDOFCHAIN - 4) fs[k++] = s;
        }
        ds = rd32(p + (per - 1) * 4);
    }
    c->nfat = k * per;
    c->fat = of_alloc((size_t)c->nfat * sizeof *c->fat + 4);
    if (!c->fat) { of_free(fs); return false; }
    for (uint32_t i = 0; i < k; i++) {
        const uint8_t *p = sector(c, fs[i]);
        for (uint32_t j = 0; j < per; j++) c->fat[i * per + j] = p ? rd32(p + j * 4) : FREESECT;
    }
    of_free(fs);
    /* the directory */
    size_t dn;
    uint8_t *dir = chain(c, rd32(buf + 0x30), 0, &dn);
    if (!dir) return false;
    c->ne = (uint32_t)(dn / 128);
    c->e = of_alloc((size_t)(c->ne + 1) * sizeof *c->e);
    if (!c->e) { of_free(dir); return false; }
    for (uint32_t i = 0; i < c->ne; i++) {
        const uint8_t *d = dir + i * 128;
        of_cfb_ent_t *e = &c->e[i];
        int nl = rd16(d + 0x40) / 2;
        if (nl > 32) nl = 32;
        int w = 0;
        for (int j = 0; j < nl; j++) {
            uint16_t ch = rd16(d + j * 2);
            if (!ch) break;
            if (w < 62) e->name[w++] = ch < 128 ? (char)ch : '?';
        }
        e->name[w] = 0;
        e->type = d[0x42];
        e->left = rd32(d + 0x44);
        e->right = rd32(d + 0x48);
        e->child = rd32(d + 0x4C);
        e->start = rd32(d + 0x74);
        e->size = rd32(d + 0x78);
        if (c->ssz == 4096) e->size |= (uint64_t)rd32(d + 0x7C) << 32;
    }
    of_free(dir);
    /* the mini stream (the root's) and its FAT */
    if (c->ne && c->e[0].type == 5) {
        size_t mn;
        c->mini = chain(c, c->e[0].start, (size_t)c->e[0].size, &mn);
        c->mini_len = c->mini ? mn : 0;
        size_t fn;
        uint32_t nmf = rd32(buf + 0x40);
        uint8_t *mf = nmf ? chain(c, rd32(buf + 0x3C), (size_t)nmf * c->ssz, &fn) : NULL;
        if (mf) {
            c->nmfat = (uint32_t)(fn / 4);
            c->mfat = of_alloc((size_t)c->nmfat * sizeof *c->mfat + 4);
            for (uint32_t i = 0; c->mfat && i < c->nmfat; i++) c->mfat[i] = rd32(mf + i * 4);
            of_free(mf);
        }
    }
    return true;
}

void of_cfb_close(of_cfb_t *c)
{
    of_free(c->fat);
    of_free(c->mfat);
    of_free(c->mini);
    of_free(c->e);
    memset(c, 0, sizeof *c);
}

/* a stream directly under the root, by name (case not minded) */
int of_cfb_find(const of_cfb_t *c, const char *name)
{
    if (!c->ne) return -1;
    uint32_t stack[64];
    int sp = 0;
    if (c->e[0].child < c->ne) stack[sp++] = c->e[0].child;
    int seen = 0;
    while (sp && seen < 4096) {
        uint32_t i = stack[--sp];
        seen++;
        if (i >= c->ne) continue;
        const of_cfb_ent_t *e = &c->e[i];
        if (!strcasecmp(e->name, name) && e->type == 2) return (int)i;
        if (e->left < c->ne && sp < 63) stack[sp++] = e->left;
        if (e->right < c->ne && sp < 63) stack[sp++] = e->right;
    }
    /* a damaged tree: any stream of that name */
    for (uint32_t i = 0; i < c->ne; i++)
        if (c->e[i].type == 2 && !strcasecmp(c->e[i].name, name)) return (int)i;
    return -1;
}

uint8_t *of_cfb_read(const of_cfb_t *c, int idx, size_t *len)
{
    if (idx < 0 || (uint32_t)idx >= c->ne) return NULL;
    const of_cfb_ent_t *e = &c->e[idx];
    size_t size = (size_t)e->size;
    if (size > c->len * 2 + 65536) return NULL;
    if (size < c->cutoff && c->mini) {
        uint8_t *b = of_alloc(size + 1);
        if (!b) return NULL;
        size_t n = 0;
        uint32_t s = e->start;
        for (uint32_t steps = 0; n < size && s < ENDOFCHAIN - 4 && steps <= c->nmfat; steps++) {
            size_t off = (size_t)s * c->mssz;
            if (off + c->mssz > c->mini_len) break;
            size_t k = size - n < c->mssz ? size - n : c->mssz;
            memcpy(b + n, c->mini + off, k);
            n += k;
            s = s < c->nmfat ? c->mfat[s] : ENDOFCHAIN;
        }
        *len = n;
        b[n] = 0;
        return b;
    }
    uint8_t *b = chain(c, e->start, size, len);
    if (b) b[*len] = 0;
    return b;
}
