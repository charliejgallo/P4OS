/*
 * P4OS - Grabador: the MP4 writer (the Video app plays them back).
 *
 * Writing is the screen recording's muxer (aos_screenrec_p4.c in the
 * firmware), with the times of the frames as they came: one video track,
 * 'mdat' first and 'moov' at the end, one sample per chunk, the samples
 * length-prefixed NALs and the parameter sets in 'avcC'. A file cut short
 * (the power going) has no 'moov' and does not play; the clips are short
 * for that reason too (gr_cfg's clip_max_s).
 */
#include "gr.h"

#include <stdlib.h>

#define TIMESCALE 1000                  /* sample times in ms */

/* ---------------------------------------------------------------- writing */

struct gr_mp4w {
    FILE     *f;
    uint32_t  mdat_at, pos;
    uint32_t  n, cap;
    uint32_t *size, *off, *dur;
    uint8_t  *key;
    uint32_t  last_ms;
    uint8_t   sps[64], pps[32];
    int       sps_len, pps_len;
    int       w, h;
    bool      err;
};
typedef struct gr_mp4w mp4_t;

static void wr(mp4_t *m, const void *p, size_t n)
{
    if (m->err) return;
    if (fwrite(p, 1, n, m->f) != n) m->err = true;
    m->pos += (uint32_t)n;
}
static void u32(mp4_t *m, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; wr(m, b, 4); }
static void u16(mp4_t *m, uint16_t v) { uint8_t b[2] = { v >> 8, v }; wr(m, b, 2); }
static void u8_(mp4_t *m, uint8_t v) { wr(m, &v, 1); }
static void tag(mp4_t *m, const char *t) { wr(m, t, 4); }
static void zeros(mp4_t *m, int n) { while (n-- > 0) u8_(m, 0); }

static long box_open(mp4_t *m, const char *t)
{
    long at = ftell(m->f);
    u32(m, 0);
    tag(m, t);
    return at;
}

static void box_close(mp4_t *m, long at)
{
    long end = ftell(m->f);
    if (at < 0 || end < 0) { m->err = true; return; }
    fseek(m->f, at, SEEK_SET);
    uint32_t sz = (uint32_t)(end - at);
    uint8_t b[4] = { sz >> 24, sz >> 16, sz >> 8, sz };
    if (fwrite(b, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, end, SEEK_SET);
}

gr_mp4w_t *gr_mp4_create(const char *path, int w, int h)
{
    mp4_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->f = fopen(path, "wb");
    if (!m->f) { free(m); return NULL; }
    m->w = w;
    m->h = h;
    long b = box_open(m, "ftyp");
    tag(m, "isom");
    u32(m, 0x200);
    tag(m, "isom"); tag(m, "iso2"); tag(m, "avc1"); tag(m, "mp41");
    box_close(m, b);
    m->mdat_at = (uint32_t)ftell(m->f);
    u32(m, 0);
    tag(m, "mdat");
    m->pos = m->mdat_at + 8;
    if (m->err) { fclose(m->f); free(m); return NULL; }
    return m;
}

static bool grow(mp4_t *m)
{
    if (m->n < m->cap) return true;
    uint32_t c = m->cap ? m->cap * 2 : 1024;
    void *a;
    if (!(a = realloc(m->size, c * 4))) return false;
    m->size = a;
    if (!(a = realloc(m->off, c * 4))) return false;
    m->off = a;
    if (!(a = realloc(m->dur, c * 4))) return false;
    m->dur = a;
    if (!(a = realloc(m->key, c))) return false;
    m->key = a;
    m->cap = c;
    return true;
}

bool gr_mp4_frame(gr_mp4w_t *m, const uint8_t *p, uint32_t len, bool key, uint32_t ms)
{
    if (!m || !grow(m)) return false;
    uint32_t start = m->pos, i = 0;
    while (i + 3 < len) {
        uint32_t s = i;
        while (s + 3 < len && !(p[s] == 0 && p[s + 1] == 0 && (p[s + 2] == 1 || (p[s + 2] == 0 && p[s + 3] == 1)))) s++;
        if (s + 3 >= len) break;
        uint32_t nal = s + (p[s + 2] == 1 ? 3 : 4);
        uint32_t e = nal;
        while (e + 3 < len && !(p[e] == 0 && p[e + 1] == 0 && (p[e + 2] == 1 || (p[e + 2] == 0 && p[e + 3] == 1)))) e++;
        if (e + 3 >= len) e = len;
        uint32_t nl = e - nal;
        int type = nl ? p[nal] & 0x1F : 0;
        if (type == 7 && nl <= sizeof m->sps) { memcpy(m->sps, p + nal, nl); m->sps_len = (int)nl; }
        else if (type == 8 && nl <= sizeof m->pps) { memcpy(m->pps, p + nal, nl); m->pps_len = (int)nl; }
        else if (nl) { u32(m, nl); wr(m, p + nal, nl); }
        i = e;
    }
    if (m->pos == start) return !m->err;
    /* the previous frame lasts until this one */
    if (m->n) m->dur[m->n - 1] = ms > m->last_ms ? ms - m->last_ms : 1;
    m->size[m->n] = m->pos - start;
    m->off[m->n] = start;
    m->dur[m->n] = 0;
    m->key[m->n] = key;
    m->n++;
    m->last_ms = ms;
    return !m->err;
}

bool gr_mp4_close(gr_mp4w_t *m, uint32_t *duration_ms)
{
    if (!m) return false;
    if (m->n) m->dur[m->n - 1] = m->n > 1 ? m->dur[m->n - 2] : 100;
    uint64_t total = 0;
    for (uint32_t i = 0; i < m->n; i++) total += m->dur[i];
    if (duration_ms) *duration_ms = (uint32_t)total;
    long end = ftell(m->f);
    fseek(m->f, m->mdat_at, SEEK_SET);
    uint32_t ms = (uint32_t)(end - m->mdat_at);
    uint8_t b[4] = { ms >> 24, ms >> 16, ms >> 8, ms };
    if (fwrite(b, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, end, SEEK_SET);

    long moov = box_open(m, "moov");
    long x = box_open(m, "mvhd");
    u32(m, 0); u32(m, 0); u32(m, 0);
    u32(m, TIMESCALE); u32(m, (uint32_t)total);
    u32(m, 0x00010000); u16(m, 0x0100); zeros(m, 10);
    u32(m, 0x00010000); zeros(m, 12); u32(m, 0x00010000); zeros(m, 12); u32(m, 0x40000000);
    zeros(m, 24); u32(m, 2);
    box_close(m, x);
    long trak = box_open(m, "trak");
    x = box_open(m, "tkhd");
    u32(m, 3); u32(m, 0); u32(m, 0); u32(m, 1); u32(m, 0); u32(m, (uint32_t)total);
    zeros(m, 8); u16(m, 0); u16(m, 0); u16(m, 0); u16(m, 0);
    u32(m, 0x00010000); zeros(m, 12); u32(m, 0x00010000); zeros(m, 12); u32(m, 0x40000000);
    u32(m, (uint32_t)m->w << 16); u32(m, (uint32_t)m->h << 16);
    box_close(m, x);
    long mdia = box_open(m, "mdia");
    x = box_open(m, "mdhd");
    u32(m, 0); u32(m, 0); u32(m, 0); u32(m, TIMESCALE); u32(m, (uint32_t)total); u16(m, 0x55C4); u16(m, 0);
    box_close(m, x);
    x = box_open(m, "hdlr");
    u32(m, 0); u32(m, 0); tag(m, "vide"); zeros(m, 12); wr(m, "P4OS camera\0", 12);
    box_close(m, x);
    long minf = box_open(m, "minf");
    x = box_open(m, "vmhd");
    u32(m, 1); zeros(m, 8);
    box_close(m, x);
    long dinf = box_open(m, "dinf");
    x = box_open(m, "dref");
    u32(m, 0); u32(m, 1);
    long url = box_open(m, "url ");
    u32(m, 1);
    box_close(m, url);
    box_close(m, x);
    box_close(m, dinf);
    long stbl = box_open(m, "stbl");
    x = box_open(m, "stsd");
    u32(m, 0); u32(m, 1);
    long avc1 = box_open(m, "avc1");
    zeros(m, 6); u16(m, 1); zeros(m, 16);
    u16(m, (uint16_t)m->w); u16(m, (uint16_t)m->h);
    u32(m, 0x00480000); u32(m, 0x00480000); u32(m, 0); u16(m, 1);
    zeros(m, 32); u16(m, 0x18); u16(m, 0xFFFF);
    long avcc = box_open(m, "avcC");
    u8_(m, 1);
    u8_(m, m->sps_len > 3 ? m->sps[1] : 66);
    u8_(m, m->sps_len > 3 ? m->sps[2] : 0);
    u8_(m, m->sps_len > 3 ? m->sps[3] : 31);
    u8_(m, 0xFF);
    u8_(m, 0xE1); u16(m, (uint16_t)m->sps_len); wr(m, m->sps, m->sps_len);
    u8_(m, 1); u16(m, (uint16_t)m->pps_len); wr(m, m->pps, m->pps_len);
    box_close(m, avcc);
    box_close(m, avc1);
    box_close(m, x);
    x = box_open(m, "stts");
    u32(m, 0);
    long cnt_at = ftell(m->f);
    u32(m, 0);
    uint32_t runs = 0;
    for (uint32_t i = 0; i < m->n;) {
        uint32_t j = i;
        while (j < m->n && m->dur[j] == m->dur[i]) j++;
        u32(m, j - i); u32(m, m->dur[i]);
        runs++;
        i = j;
    }
    long here = ftell(m->f);
    fseek(m->f, cnt_at, SEEK_SET);
    uint8_t rb[4] = { runs >> 24, runs >> 16, runs >> 8, runs };
    if (fwrite(rb, 1, 4, m->f) != 4) m->err = true;
    fseek(m->f, here, SEEK_SET);
    box_close(m, x);
    x = box_open(m, "stss");
    u32(m, 0);
    uint32_t keys = 0;
    for (uint32_t i = 0; i < m->n; i++) keys += m->key[i];
    u32(m, keys);
    for (uint32_t i = 0; i < m->n; i++) if (m->key[i]) u32(m, i + 1);
    box_close(m, x);
    x = box_open(m, "stsc");
    u32(m, 0); u32(m, 1); u32(m, 1); u32(m, 1); u32(m, 1);
    box_close(m, x);
    x = box_open(m, "stsz");
    u32(m, 0); u32(m, 0); u32(m, m->n);
    for (uint32_t i = 0; i < m->n; i++) u32(m, m->size[i]);
    box_close(m, x);
    x = box_open(m, "stco");
    u32(m, 0); u32(m, m->n);
    for (uint32_t i = 0; i < m->n; i++) u32(m, m->off[i]);
    box_close(m, x);
    box_close(m, stbl);
    box_close(m, minf);
    box_close(m, mdia);
    box_close(m, trak);
    box_close(m, moov);
    bool ok = !m->err && fclose(m->f) == 0;
    free(m->size);
    free(m->off);
    free(m->dur);
    free(m->key);
    free(m);
    return ok;
}
