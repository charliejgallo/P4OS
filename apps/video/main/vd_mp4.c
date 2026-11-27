/*
 * P4OS - Video: MP4 files with H.264 (since 0.35), next to the AVIs.
 *
 * What Grabador records, and any MP4 as simple: the first video track's
 * avcC, stsz, stco or co64, stts, stss and stsc. Each sample comes out as
 * Annex B (start codes), with the SPS and PPS before each key frame, which is
 * what aos_hal_h264_decode takes. The decoder is software on the board
 * (Constrained Baseline only, ~24 fps at 640x480, ~8.6 at 1280x720), so a
 * bigger or Main/High file plays slowly or not at all; the simulator's
 * libavcodec plays anything.
 */
#include "vd_mp4.h"

#include <stdlib.h>
#include <string.h>

struct vd_mp4 {
    FILE     *f;
    uint32_t  n;
    uint32_t *size, *ms;
    uint64_t *off;
    uint8_t  *key;
    uint8_t   ps[160];              /* start code + SPS, start code + PPS */
    int       ps_len;
    int       w, h;
    uint32_t  total;
    uint32_t  max_size;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the box 'want' among [at, end): its payload's place and length */
static bool find_box(FILE *f, uint64_t at, uint64_t end, const char *want, uint64_t *pay, uint64_t *len)
{
    while (at + 8 <= end) {
        uint8_t h[16];
        if (fseek(f, (long)at, SEEK_SET) || fread(h, 1, 8, f) != 8) return false;
        uint64_t sz = be32(h), hl = 8;
        if (sz == 1) {
            if (fread(h + 8, 1, 8, f) != 8) return false;
            sz = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
            hl = 16;
        } else if (sz == 0) {
            sz = end - at;
        }
        if (sz < hl || at + sz > end) return false;
        if (!memcmp(h + 4, want, 4)) { *pay = at + hl; *len = sz - hl; return true; }
        at += sz;
    }
    return false;
}

static uint8_t *read_box(FILE *f, uint64_t at, uint64_t len)
{
    if (len > 16 * 1024 * 1024) return NULL;
    uint8_t *b = malloc(len ? (size_t)len : 1);
    if (b && (fseek(f, (long)at, SEEK_SET) || fread(b, 1, (size_t)len, f) != len)) { free(b); return NULL; }
    return b;
}

vd_mp4_t *vd_mp4_open(const char *path, int *w, int *h, uint32_t *duration_ms, uint32_t *frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    uint64_t end = (uint64_t)ftell(f);
    vd_mp4_t *r = calloc(1, sizeof *r);
    uint64_t moov, ml, trak, tl, mdia, dl, minf, il, stbl, sl, p, l;
    uint8_t *stsz = NULL, *stco = NULL, *stts = NULL, *stss = NULL, *stsc = NULL, *stsd = NULL;
    bool co64 = false;
    if (!r || !find_box(f, 0, end, "moov", &moov, &ml) || !find_box(f, moov, moov + ml, "trak", &trak, &tl) ||
        !find_box(f, trak, trak + tl, "mdia", &mdia, &dl) || !find_box(f, mdia, mdia + dl, "minf", &minf, &il) ||
        !find_box(f, minf, minf + il, "stbl", &stbl, &sl))
        goto fail;
    if (!find_box(f, stbl, stbl + sl, "stsz", &p, &l) || !(stsz = read_box(f, p, l)) || l < 12) goto fail;
    if (find_box(f, stbl, stbl + sl, "stco", &p, &l)) stco = read_box(f, p, l);
    else if (find_box(f, stbl, stbl + sl, "co64", &p, &l)) { stco = read_box(f, p, l); co64 = true; }
    if (!stco) goto fail;
    if (find_box(f, stbl, stbl + sl, "stts", &p, &l)) stts = read_box(f, p, l);
    if (find_box(f, stbl, stbl + sl, "stss", &p, &l)) stss = read_box(f, p, l);
    if (find_box(f, stbl, stbl + sl, "stsc", &p, &l)) stsc = read_box(f, p, l);
    uint64_t stsd_l = 0;
    if (find_box(f, stbl, stbl + sl, "stsd", &p, &stsd_l)) stsd = read_box(f, p, stsd_l);
    r->n = be32(stsz + 8);
    if (!r->n || r->n > 1000000) goto fail;
    r->size = calloc(r->n, 4);
    r->ms = calloc(r->n, 4);
    r->off = calloc(r->n, 8);
    r->key = calloc(r->n, 1);
    if (!r->size || !r->ms || !r->off || !r->key) goto fail;
    uint32_t fixed = be32(stsz + 4);
    for (uint32_t i = 0; i < r->n; i++) {
        r->size[i] = fixed ? fixed : be32(stsz + 12 + 4 * i);
        if (r->size[i] > r->max_size) r->max_size = r->size[i];
    }
    /* chunks into samples (stsc), one sample a chunk when it says nothing */
    uint32_t nchunks = be32(stco + 4);
    uint32_t si = 0;
    uint32_t nstsc = stsc ? be32(stsc + 4) : 0;
    for (uint32_t c = 0; c < nchunks && si < r->n; c++) {
        uint32_t per = 1;
        for (uint32_t k = 0; k < nstsc; k++)
            if (be32(stsc + 8 + 12 * k) <= c + 1) per = be32(stsc + 8 + 12 * k + 4);
        uint64_t o = co64 ? (uint64_t)be32(stco + 8 + 8 * c) << 32 | be32(stco + 12 + 8 * c) : be32(stco + 8 + 4 * c);
        for (uint32_t k = 0; k < per && si < r->n; k++) { r->off[si] = o; o += r->size[si]; si++; }
    }
    /* times */
    uint32_t t = 0;
    if (stts) {
        uint32_t runs = be32(stts + 4), i = 0;
        for (uint32_t k = 0; k < runs && i < r->n; k++)
            for (uint32_t c = be32(stts + 8 + 8 * k), d = be32(stts + 12 + 8 * k); c && i < r->n; c--) { r->ms[i++] = t; t += d; }
    }
    r->total = t;
    if (stss) {
        uint32_t n = be32(stss + 4);
        for (uint32_t k = 0; k < n; k++) { uint32_t s = be32(stss + 8 + 4 * k); if (s >= 1 && s <= r->n) r->key[s - 1] = 1; }
    } else {
        memset(r->key, 1, r->n);
    }
    /* the size from the sample entry (stsd: 8 bytes, then the entry's size
     * and 'avc1', 24 more, width and height) */
    if (stsd && stsd_l > 44 && !memcmp(stsd + 12, "avc1", 4)) {
        r->w = stsd[40] << 8 | stsd[41];
        r->h = stsd[42] << 8 | stsd[43];
    }
    /* the parameter sets from avcC, inside it */
    for (uint64_t i = 8; stsd && i + 8 < stsd_l; i++) {
        if (memcmp(stsd + i, "avcC", 4)) continue;
        const uint8_t *a = stsd + i + 4;
        int nsps = a[5] & 0x1F, at = 6;
        for (int k = 0; k < nsps && at + 2 < (int)(stsd_l - i); k++) {
            int sl2 = a[at] << 8 | a[at + 1];
            if (r->ps_len + 4 + sl2 <= (int)sizeof r->ps) {
                memcpy(r->ps + r->ps_len, "\0\0\0\1", 4);
                memcpy(r->ps + r->ps_len + 4, a + at + 2, (size_t)sl2);
                r->ps_len += 4 + sl2;
            }
            at += 2 + sl2;
        }
        int npps = a[at++];
        for (int k = 0; k < npps && at + 2 < (int)(stsd_l - i); k++) {
            int pl = a[at] << 8 | a[at + 1];
            if (r->ps_len + 4 + pl <= (int)sizeof r->ps) {
                memcpy(r->ps + r->ps_len, "\0\0\0\1", 4);
                memcpy(r->ps + r->ps_len + 4, a + at + 2, (size_t)pl);
                r->ps_len += 4 + pl;
            }
            at += 2 + pl;
        }
        break;
    }
    free(stsz); free(stco); free(stts); free(stss); free(stsc); free(stsd);
    r->f = f;
    if (w) *w = r->w;
    if (h) *h = r->h;
    if (duration_ms) *duration_ms = r->total;
    if (frames) *frames = r->n;
    return r;
fail:
    free(stsz); free(stco); free(stts); free(stss); free(stsc); free(stsd);
    if (r) { free(r->size); free(r->ms); free(r->off); free(r->key); free(r); }
    fclose(f);
    return NULL;
}

int vd_mp4_sample(vd_mp4_t *r, uint32_t i, uint8_t *buf, int max, uint32_t *ms, bool *key)
{
    if (!r || i >= r->n) return -1;
    if (ms) *ms = r->ms[i];
    if (key) *key = r->key[i];
    int o = 0;
    if (r->key[i] && r->ps_len && r->ps_len < max) { memcpy(buf, r->ps, (size_t)r->ps_len); o = r->ps_len; }
    uint32_t left = r->size[i];
    if (fseek(r->f, (long)r->off[i], SEEK_SET)) return -1;
    /* length-prefixed NALs into start codes */
    while (left >= 4) {
        uint8_t l4[4];
        if (fread(l4, 1, 4, r->f) != 4) return -1;
        uint32_t nl = be32(l4);
        left -= 4;
        if (nl > left || o + 4 + (int)nl > max) return -1;
        memcpy(buf + o, "\0\0\0\1", 4);
        if (fread(buf + o + 4, 1, nl, r->f) != nl) return -1;
        o += 4 + (int)nl;
        left -= nl;
    }
    return o;
}

void vd_mp4_free(vd_mp4_t *r)
{
    if (!r) return;
    fclose(r->f);
    free(r->size);
    free(r->ms);
    free(r->off);
    free(r->key);
    free(r);
}

uint32_t vd_mp4_key_at(const vd_mp4_t *r, uint32_t i)
{
    if (!r || !r->n) return 0;
    if (i >= r->n) i = r->n - 1;
    while (i > 0 && !r->key[i]) i--;
    return i;
}

size_t vd_mp4_buf_size(const vd_mp4_t *r)
{
    /* the start codes in place of the lengths, and the parameter sets */
    return r ? (size_t)r->max_size + sizeof r->ps + 64 : 0;
}

uint32_t vd_mp4_frame_ms(const vd_mp4_t *r, uint32_t i)
{
    return r && i < r->n ? r->ms[i] : r ? r->total : 0;
}
