/* P4OS (from AmoledOS) - Video: MJPEG-in-AVI reader. See vd_avi.h. */
#include "vd_avi.h"

#include <stdlib.h>
#include <string.h>

#define HDRL_MAX    (256 * 1024)        /* the header list, read whole */
#define FRAME_MAX   (4 * 1024 * 1024)   /* one JPEG; anything bigger is not a frame */
#define IX_MAX      (4 * 1024 * 1024)   /* one OpenDML 'ix00' chunk, read whole */
#define IDX1_BLOCK  1024                /* idx1 entries per read */
#define READ_ALIGN  128                 /* the SDMMC DMA's line (aos_hal_io_alloc) */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

/* Reads at 'pos', seeking only when the file is not there already.
 *
 * Measured on the watch (2026-09-16): an fseek to the position the file is
 * already at is NOT free. FatFS walks the cluster chain from the start of
 * the file for any seek that is not strictly forward, so a seek per frame
 * cost 41 ms at frame 30 and 94 ms at frame 170 of a 2.3 MB file, more
 * than the decode itself. Sequential reading needs no seek at all. */
static bool read_at(vd_avi_t *a, long pos, void *dst, size_t n)
{
    if (a->file_pos != pos) {
        if (fseek(a->file, pos, SEEK_SET) != 0) {
            a->file_pos = -1;
            return false;
        }
        a->file_pos = pos;
    }
    size_t got = fread(dst, 1, n, a->file);
    a->file_pos = got == n ? a->file_pos + (long)got : -1;
    return got == n;
}

/* ---- the headers ---------------------------------------------------------- */

typedef struct {
    int            strl_no;         /* streams seen so far */
    bool           have_video;
    const uint8_t *indx;            /* inside the hdrl buffer */
    uint32_t       indx_len;
    uint32_t       avih_frames, dmlh_frames, strh_length;
    uint32_t       us_per_frame;
    char           codec[5];
} hdr_t;

static bool is_mjpeg(const char *c)
{
    static const char *const OK[] = { "MJPG", "JPEG", "AVRN", "DMB1", "MJPA" };
    char up[4];
    for (int i = 0; i < 4; i++) {       /* (strncasecmp is not in the firmware's table) */
        up[i] = c[i] >= 'a' && c[i] <= 'z' ? (char)(c[i] - 'a' + 'A') : c[i];
    }
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) {
        if (memcmp(up, OK[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_list(vd_avi_t *a, hdr_t *h, const uint8_t *p, uint32_t n, bool in_strl);

/* One chunk of the header list. */
static void parse_chunk(vd_avi_t *a, hdr_t *h, const uint8_t *id, const uint8_t *body,
                        uint32_t size, bool in_strl, bool *strl_is_video)
{
    if (memcmp(id, "LIST", 4) == 0 && size >= 4) {
        bool strl = memcmp(body, "strl", 4) == 0;
        parse_list(a, h, body + 4, size - 4, strl);
        if (strl) {
            h->strl_no++;
        }
    } else if (memcmp(id, "avih", 4) == 0 && size >= 40) {
        h->us_per_frame = rd32(body + 0);
        h->avih_frames  = rd32(body + 16);
        a->width        = (uint16_t)rd32(body + 32);
        a->height       = (uint16_t)rd32(body + 36);
    } else if (memcmp(id, "dmlh", 4) == 0 && size >= 4) {
        h->dmlh_frames = rd32(body);
    } else if (in_strl && memcmp(id, "strh", 4) == 0 && size >= 36) {
        /* The first video stream is the one played; any other is ignored. */
        if (memcmp(body, "vids", 4) == 0 && !h->have_video) {
            h->have_video  = true;
            *strl_is_video = true;
            a->stream[0]   = (char)('0' + h->strl_no / 10);
            a->stream[1]   = (char)('0' + h->strl_no % 10);
            a->scale       = rd32(body + 20);
            a->rate        = rd32(body + 24);
            h->strh_length = rd32(body + 32);
            memcpy(h->codec, body + 4, 4);
        }
    } else if (in_strl && *strl_is_video && memcmp(id, "strf", 4) == 0 && size >= 20) {
        int32_t w = (int32_t)rd32(body + 4), hh = (int32_t)rd32(body + 8);
        if (w > 0 && !a->width) {
            a->width = (uint16_t)w;
        }
        if (hh != 0 && !a->height) {
            a->height = (uint16_t)(hh < 0 ? -hh : hh);
        }
        memcpy(h->codec, body + 16, 4);   /* biCompression says it better than fccHandler */
    } else if (in_strl && *strl_is_video && memcmp(id, "indx", 4) == 0) {
        h->indx     = body;
        h->indx_len = size;
    }
}

static void parse_list(vd_avi_t *a, hdr_t *h, const uint8_t *p, uint32_t n, bool in_strl)
{
    bool strl_is_video = false;
    for (uint32_t i = 0; i + 8 <= n; ) {
        uint32_t size = rd32(p + i + 4);
        if (size > n - i - 8) {
            size = n - i - 8;       /* a list cut short: take what is there */
        }
        parse_chunk(a, h, p + i, p + i + 8, size, in_strl, &strl_is_video);
        i += 8 + size + (size & 1);
    }
}

/* ---- the index -------------------------------------------------------------- */

typedef struct {
    uint32_t cap;
    long     file_len;
} build_t;

static bool push(vd_avi_t *a, build_t *b, uint64_t off, uint32_t size)
{
    if (off + 8 + size > (uint64_t)b->file_len || size > FRAME_MAX) {
        return true;                /* past the end of a cut file: dropped, not fatal */
    }
    if (a->frames == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 1024;
        vd_frame_t *grown = realloc(a->index, cap * sizeof(vd_frame_t));
        if (!grown) {
            return false;
        }
        a->index = grown;
        b->cap   = cap;
    }
    a->index[a->frames].off  = (uint32_t)off;
    a->index[a->frames].size = size;
    a->frames++;
    if (size > a->max_size) {
        a->max_size = size;
    }
    return true;
}

static bool is_video_id(const vd_avi_t *a, const uint8_t *id)
{
    return id[0] == (uint8_t)a->stream[0] && id[1] == (uint8_t)a->stream[1] &&
           id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
}

/* OpenDML: the index of indexes, then every 'ix00' it points to. */
static bool index_odml(vd_avi_t *a, build_t *b, const hdr_t *h)
{
    if (!h->indx || h->indx_len < 24) {
        return false;
    }
    const uint8_t *x = h->indx;
    uint32_t n = rd32(x + 4);
    if (rd16(x) != 4 || x[3] != 0 /* AVI_INDEX_OF_INDEXES */ || n == 0 || n > (h->indx_len - 24) / 16) {
        return false;
    }
    for (uint32_t e = 0; e < n; e++) {
        uint64_t at  = rd64(x + 24 + e * 16);
        uint8_t  head[32];
        if (at + sizeof head > (uint64_t)b->file_len || !read_at(a, (long)at, head, sizeof head)) {
            return false;
        }
        uint32_t size    = rd32(head + 4);
        uint32_t entries = rd32(head + 12);
        uint64_t base    = rd64(head + 20);
        if (rd16(head + 8) != 2 || head[11] != 1 /* AVI_INDEX_OF_CHUNKS */ ||
            size < 24 || size > IX_MAX || entries > (size - 24) / 8) {
            return false;
        }
        uint8_t *list = malloc((size_t)entries * 8 + 1);
        if (!list) {
            return false;
        }
        bool ok = read_at(a, (long)at + 32, list, (size_t)entries * 8);
        for (uint32_t k = 0; ok && k < entries; k++) {
            uint64_t data = base + rd32(list + k * 8);
            ok = data >= 8 && push(a, b, data - 8, rd32(list + k * 8 + 4) & 0x7FFFFFFFu);
        }
        free(list);
        if (!ok) {
            return false;
        }
    }
    return a->frames > 0;
}

/* The classic idx1. */
static bool index_idx1(vd_avi_t *a, build_t *b, long idx1_pos, uint32_t idx1_len, long movi_pos)
{
    if (idx1_pos <= 0 || idx1_len < 16) {
        return false;
    }
    uint8_t *blk = malloc(IDX1_BLOCK * 16);
    if (!blk) {
        return false;
    }
    uint32_t total = idx1_len / 16;
    long     base  = -1;
    bool     ok    = true;
    for (uint32_t done = 0; ok && done < total; ) {
        uint32_t n = total - done < IDX1_BLOCK ? total - done : IDX1_BLOCK;
        ok = read_at(a, idx1_pos + (long)done * 16, blk, (size_t)n * 16);
        for (uint32_t k = 0; ok && k < n; k++) {
            const uint8_t *e = blk + k * 16;
            if (!is_video_id(a, e)) {
                continue;
            }
            uint32_t off = rd32(e + 8);
            if (base < 0) {
                /* Relative to the 'movi' fourcc, or to the file: the chunk
                 * id at the place each would mean says which. */
                uint8_t id[4];
                base = movi_pos + 8;
                if (!(read_at(a, base + (long)off, id, 4) && memcmp(id, e, 4) == 0) &&
                    read_at(a, (long)off, id, 4) && memcmp(id, e, 4) == 0) {
                    base = 0;
                }
            }
            ok = push(a, b, (uint64_t)base + off, rd32(e + 12));
        }
        done += n;
    }
    free(blk);
    return ok && a->frames > 0;
}

/* No index at all: every chunk of one 'movi', header by header. */
static bool scan_movi(vd_avi_t *a, build_t *b, long pos, long end)
{
    while (pos + 8 <= end) {
        uint8_t head[12];
        if (!read_at(a, pos, head, 8)) {
            return false;
        }
        uint32_t size = rd32(head + 4);
        if (memcmp(head, "LIST", 4) == 0) {
            pos += 12;              /* 'rec ' groups: walk straight in */
            continue;
        }
        if (is_video_id(a, head) && !push(a, b, (uint64_t)pos, size)) {
            return false;
        }
        pos += 8 + (long)size + (size & 1);
    }
    return true;
}

static bool index_scan(vd_avi_t *a, build_t *b, long movi_pos, long movi_end)
{
    if (!scan_movi(a, b, movi_pos + 12, movi_end)) {
        return false;
    }
    /* OpenDML without its index: the frames go on in 'AVIX' RIFFs. */
    long riff = movi_end + (movi_end & 1);
    for (;;) {
        uint8_t head[12];
        long    at = riff;
        while (at + 12 <= b->file_len && read_at(a, at, head, 12) && memcmp(head, "RIFF", 4) != 0) {
            at += 8 + (long)rd32(head + 4);     /* idx1, JUNK: over them */
        }
        if (at + 12 > b->file_len || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "AVIX", 4) != 0) {
            break;
        }
        long end = at + 8 + (long)rd32(head + 4);
        for (long p = at + 12; p + 12 <= end; ) {
            if (!read_at(a, p, head, 12)) {
                return a->frames > 0;
            }
            uint32_t size = rd32(head + 4);
            if (memcmp(head, "LIST", 4) == 0 && memcmp(head + 8, "movi", 4) == 0) {
                scan_movi(a, b, p + 12, p + 8 + (long)size);
            }
            p += 8 + (long)size + (size & 1);
        }
        riff = end + (end & 1);
    }
    return a->frames > 0;
}

/* ---- open ------------------------------------------------------------------- */

bool vd_avi_open(vd_avi_t *a, const char *path, bool want_index)
{
    memset(a, 0, sizeof *a);
    a->file = fopen(path, "rb");
    if (!a->file) {
        return false;
    }
    /* No setvbuf: a frame is one fread of tens of KB, which newlib passes
     * straight to the filesystem in one go, so stdio's small buffer only
     * ever serves the headers. */
    build_t b = { 0 };
    uint8_t riff[12];
    if (fseek(a->file, 0, SEEK_END) != 0 || (b.file_len = ftell(a->file)) < 12) {
        goto fail;
    }
    a->file_pos = b.file_len;
    if (!read_at(a, 0, riff, 12) || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "AVI ", 4) != 0) {
        goto fail;
    }

    long     riff_end = 8 + (long)rd32(riff + 4);
    long     movi_pos = 0, movi_end = 0, idx1_pos = 0;
    uint32_t idx1_len = 0;
    uint8_t *hdrl     = NULL;
    hdr_t    h        = { 0 };
    if (riff_end > b.file_len) {
        riff_end = b.file_len;      /* a file cut short, or a RIFF that says 0 */
    }
    for (long pos = 12; pos + 8 <= riff_end; ) {
        uint8_t head[12];
        if (!read_at(a, pos, head, 8)) {
            break;
        }
        uint32_t size = rd32(head + 4);
        if (memcmp(head, "LIST", 4) == 0 && size >= 4 && read_at(a, pos + 8, head + 8, 4)) {
            if (memcmp(head + 8, "hdrl", 4) == 0 && !hdrl && size - 4 <= HDRL_MAX) {
                hdrl = malloc(size - 4 + 1);
                if (!hdrl || !read_at(a, pos + 12, hdrl, size - 4)) {
                    free(hdrl);
                    goto fail;
                }
                parse_list(a, &h, hdrl, size - 4, false);
            } else if (memcmp(head + 8, "movi", 4) == 0 && !movi_pos) {
                movi_pos = pos;
                movi_end = pos + 8 + (long)size;
                if (movi_end > b.file_len) {
                    movi_end = b.file_len;
                }
            }
        } else if (memcmp(head, "idx1", 4) == 0) {
            idx1_pos = pos + 8;
            idx1_len = size;
        }
        pos += 8 + (long)size + (size & 1);
    }

    if (!h.have_video || !movi_pos || !a->width || !a->height) {
        free(hdrl);
        goto fail;
    }
    if (!is_mjpeg(h.codec)) {
        free(hdrl);
        goto fail;
    }
    if (!a->rate || !a->scale) {
        /* no strh rate: the main header's period, in microseconds */
        a->rate  = 1000000;
        a->scale = h.us_per_frame ? h.us_per_frame : 66667;
    }

    /* The first frame, for the list's picture: the first video chunk of 'movi'. */
    for (long pos = movi_pos + 12, n = 0; pos + 8 <= movi_end && n < 64; n++) {
        uint8_t head[8];
        if (!read_at(a, pos, head, 8)) {
            break;
        }
        uint32_t size = rd32(head + 4);
        if (memcmp(head, "LIST", 4) == 0) {
            pos += 12;
            continue;
        }
        if (is_video_id(a, head) && size > 0) {
            a->first.off  = (uint32_t)pos;
            a->first.size = size;
            break;
        }
        pos += 8 + (long)size + (size & 1);
    }

    if (!want_index) {
        a->frames = h.dmlh_frames ? h.dmlh_frames : h.strh_length ? h.strh_length : h.avih_frames;
        free(hdrl);
        return a->first.size > 0;
    }
    bool ok = index_odml(a, &b, &h);
    if (!ok) {
        a->frames = 0;
        a->max_size = 0;
        ok = index_idx1(a, &b, idx1_pos, idx1_len, movi_pos);
    }
    if (!ok) {
        a->frames = 0;
        a->max_size = 0;
        ok = index_scan(a, &b, movi_pos, movi_end);
    }
    free(hdrl);
    if (!ok) {
        goto fail;
    }
    return true;

fail:
    vd_avi_close(a);
    return false;
}

void vd_avi_close(vd_avi_t *a)
{
    if (a->file) {
        fclose(a->file);
        a->file = NULL;
    }
    free(a->index);
    a->index = NULL;
}

/* ---- time ------------------------------------------------------------------- */

uint32_t vd_avi_frame_ms(const vd_avi_t *a, uint32_t frame)
{
    return a->rate ? (uint32_t)((uint64_t)frame * a->scale * 1000u / a->rate) : 0;
}

uint32_t vd_avi_frame_at(const vd_avi_t *a, uint64_t ms)
{
    if (!a->scale || !a->frames) {
        return 0;
    }
    uint64_t f = ms * a->rate / ((uint64_t)a->scale * 1000u);
    return f >= a->frames ? a->frames - 1 : (uint32_t)f;
}

uint32_t vd_avi_duration_ms(const vd_avi_t *a)
{
    return vd_avi_frame_ms(a, a->frames);
}

/* ---- frames ----------------------------------------------------------------- */

size_t vd_avi_buf_size(const vd_avi_t *a)
{
    return ((size_t)a->max_size + 8 + 2 + READ_ALIGN + 3) & ~(size_t)3;
}

int vd_avi_read(vd_avi_t *a, uint32_t i, uint8_t *buf, const uint8_t **jpeg)
{
    if (!a->file || !a->index || i >= a->frames) {
        return -1;
    }
    const vd_frame_t *f = &a->index[i];
    if (f->size == 0) {
        return 0;
    }
    /* Header, data and the pad byte in one read, which leaves the file at
     * the next frame's header. Placed at buf + (offset % 128): FatFS reads
     * whole sectors straight into the caller's buffer, and this lands them
     * on 128-byte lines, which the SDMMC DMA takes without bouncing them
     * through its own buffer (docs/MEMORY.md, "The card"). */
    size_t   need = 8 + f->size + (f->size & 1);
    uint8_t *dst  = buf + (f->off % READ_ALIGN);
    if (!read_at(a, (long)f->off, dst, need) &&
        !(need > 8 + f->size && read_at(a, (long)f->off, dst, 8 + f->size))) {
        return -1;          /* (the second try: a last frame with no pad byte) */
    }
    if (!is_video_id(a, dst)) {
        return -1;
    }
    *jpeg = dst + 8;
    return (int)f->size;
}
