/*
 * P4OS - The picture loader of the built-in apps (see aos_app_image.h).
 *
 * One thread for every app, started when there is work and gone when the
 * queue empties: decoding is what makes a gallery of hundreds scroll or not,
 * and none of it may happen in LVGL's task (a 12 MP JPEG is 3-4 MB to read
 * off the card before the decoder even starts). Jobs are taken urgent first
 * and then newest first, because the newest request is what just scrolled
 * into view; the ones that scrolled out again are cancelled by the app
 * before the thread gets to them.
 *
 * Thumbnails can also be kept on the card (cache_dir). The key is the path,
 * the file's size and date and the size asked for, so a photo that changes
 * gets a new one; the second visit to a folder reads 60 KB per tile instead
 * of decoding megabytes.
 */
#include "aos_app_image.h"
#include "aos_hal.h"
#include "aos_text_safe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define QMAX        128         /* queued jobs, every owner together */
#define DMAX        48          /* finished and not taken yet        */
#define PATH_LEN    256
#define THUMB_MAGIC 0x31544F41u /* "AOT1" */

enum { KIND_NONE = 0, KIND_MALLOC, KIND_HAL };

typedef struct {
    int       owner;
    uintptr_t tag;
    uint32_t  seq;
    bool      urgent, fill;
    uint32_t  offset, size;
    int       w, h;
    bool      id3;
    char      path[PATH_LEN];
    char      cache[160];
} job_t;

typedef struct {
    uint32_t magic;
    uint16_t w, h, src_w, src_h;
} thumb_hdr_t;

AOS_BSS_PSRAM static job_t s_q[QMAX];
static int          s_nq;
static img_result_t s_done[DMAX];
static int          s_done_owner[DMAX];
static int          s_nd;
static void        *s_mux;
static bool         s_thread_on;
static uint32_t     s_seq;
static int          s_run_owner;        /* the job on the thread, 0 if none */
static bool         s_run_cancelled;

/* ---- small things ---------------------------------------------------------- */

int img_name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)((ea - a) - (eb - b));
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* A base letter and a combining mark, composed into Latin-1 (all of it is
 * in Inter). */
static uint32_t compose(uint32_t base, uint32_t mark)
{
    static const struct { char base; uint16_t mark; uint8_t out; } T[] = {
        {'A',0x300,0xC0},{'E',0x300,0xC8},{'I',0x300,0xCC},{'O',0x300,0xD2},{'U',0x300,0xD9},
        {'a',0x300,0xE0},{'e',0x300,0xE8},{'i',0x300,0xEC},{'o',0x300,0xF2},{'u',0x300,0xF9},
        {'A',0x301,0xC1},{'E',0x301,0xC9},{'I',0x301,0xCD},{'O',0x301,0xD3},{'U',0x301,0xDA},
        {'Y',0x301,0xDD},{'a',0x301,0xE1},{'e',0x301,0xE9},{'i',0x301,0xED},{'o',0x301,0xF3},
        {'u',0x301,0xFA},{'y',0x301,0xFD},
        {'A',0x302,0xC2},{'E',0x302,0xCA},{'I',0x302,0xCE},{'O',0x302,0xD4},{'U',0x302,0xDB},
        {'a',0x302,0xE2},{'e',0x302,0xEA},{'i',0x302,0xEE},{'o',0x302,0xF4},{'u',0x302,0xFB},
        {'A',0x303,0xC3},{'N',0x303,0xD1},{'O',0x303,0xD5},{'a',0x303,0xE3},{'n',0x303,0xF1},
        {'o',0x303,0xF5},
        {'A',0x308,0xC4},{'E',0x308,0xCB},{'I',0x308,0xCF},{'O',0x308,0xD6},{'U',0x308,0xDC},
        {'a',0x308,0xE4},{'e',0x308,0xEB},{'i',0x308,0xEF},{'o',0x308,0xF6},{'u',0x308,0xFC},
        {'y',0x308,0xFF},{'A',0x30A,0xC5},{'a',0x30A,0xE5},{'C',0x327,0xC7},{'c',0x327,0xE7},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if ((uint32_t)(unsigned char)T[i].base == base && T[i].mark == mark) return T[i].out;
    }
    return 0;
}

size_t img_text(char *out, size_t len, const char *in)
{
    char tmp[512];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 4 < sizeof tmp;) {
        /* a combining mark (U+0300..U+036F is 0xCC 0x80 .. 0xCD 0xAF) */
        if ((p[0] == 0xCC || (p[0] == 0xCD && p[1] <= 0xAF)) && p[1] >= 0x80 && p[1] <= 0xBF) {
            uint32_t mark = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            uint32_t c = o ? compose((unsigned char)tmp[o - 1], mark) : 0;
            if (c) {
                tmp[o - 1] = (char)(0xC0 | (c >> 6));
                tmp[o++] = (char)(0x80 | (c & 0x3F));
            }
            p += 2;                     /* one Inter has no use for: dropped */
            continue;
        }
        tmp[o++] = (char)*p++;
    }
    tmp[o] = '\0';
    return aos_text_safe(out, len, tmp);
}

uint16_t *img_downscale(const uint16_t *src, int sw, int sh, int dw, int dh)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
    uint16_t *out = malloc((size_t)dw * (size_t)dh * 2u);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint16_t *row = src + (size_t)yy * (size_t)sw;
                for (int xx = x0; xx < x1; xx++) {
                    uint16_t p = row[xx];
                    r += p >> 11;
                    g += (p >> 5) & 63;
                    b += p & 31;
                    n++;
                }
            }
            out[(size_t)y * (size_t)dw + (size_t)x] =
                (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
    return out;
}

uint32_t img_average(const uint16_t *px, int w, int h)
{
    if (!px || w <= 0 || h <= 0) return 0;
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            uint16_t p = px[(size_t)y * (size_t)w + (size_t)x];
            r += (p >> 11) * 255 / 31;
            g += ((p >> 5) & 63) * 255 / 63;
            b += (p & 31) * 255 / 31;
            n++;
        }
    }
    return (uint32_t)((r / n) << 16 | (g / n) << 8 | (b / n));
}

void img_free(img_result_t *r)
{
    if (!r || !r->px) return;
    if (r->kind_ == KIND_HAL) aos_hal_image_free(r->px);
    else free(r->px);
    r->px = NULL;
    r->kind_ = KIND_NONE;
}

/* ---- the card's thumbnails ------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static bool cache_name(const job_t *j, char *out, size_t len)
{
    struct stat st;
    if (stat(j->path, &st) != 0) return false;
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv(h, j->path, strlen(j->path));
    int64_t facts[6] = { (int64_t)st.st_size, (int64_t)st.st_mtime, j->offset, j->size,
                         j->w * 65536 + j->h, j->fill };
    h = fnv(h, facts, sizeof facts);
    return snprintf(out, len, "%s/%016llx.t", j->cache, (unsigned long long)h) < (int)len;
}

static bool cache_read(const char *file, const job_t *j, img_result_t *r)
{
    FILE *f = fopen(file, "rb");
    if (!f) return false;
    thumb_hdr_t hd;
    bool ok = fread(&hd, sizeof hd, 1, f) == 1 && hd.magic == THUMB_MAGIC &&
              hd.w && hd.h && hd.w <= j->w && hd.h <= j->h;
    uint16_t *px = NULL;
    if (ok) {
        size_t n = (size_t)hd.w * hd.h;
        px = malloc(n * 2u);
        ok = px && fread(px, 2, n, f) == n;
    }
    fclose(f);
    if (!ok) {
        free(px);
        return false;
    }
    r->px = px;
    r->kind_ = KIND_MALLOC;
    r->w = hd.w;
    r->h = hd.h;
    r->src_w = hd.src_w;
    r->src_h = hd.src_h;
    return true;
}

static void cache_write(const char *file, const img_result_t *r)
{
    char tmp[200];
    if (snprintf(tmp, sizeof tmp, "%s~", file) >= (int)sizeof tmp) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    thumb_hdr_t hd = { THUMB_MAGIC, (uint16_t)r->w, (uint16_t)r->h,
                       (uint16_t)r->src_w, (uint16_t)r->src_h };
    size_t n = (size_t)r->w * (size_t)r->h;
    bool ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(r->px, 2, n, f) == n;
    ok = fclose(f) == 0 && ok;
    /* written whole or not at all: a half thumbnail would be read forever */
    if (!ok || rename(tmp, file) != 0) remove(tmp);
}

/* ---- the picture inside an MP3 --------------------------------------------- */

static uint32_t be32(const uint8_t *b, bool syncsafe)
{
    if (syncsafe) return (uint32_t)(b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* Where the APIC frame's picture is in an ID3v2.3/2.4 tag. The player's
 * parser (the HAL's) finds it too, but only for the track that plays; this
 * is for the rows and the album page. */
static bool find_apic(const char *path, uint32_t *off, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[10];
    bool ok = false;
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0 || h[3] < 3 || h[3] > 4) goto done;
    bool v4 = h[3] == 4;
    uint32_t end = 10 + be32(h + 6, true), pos = 10;
    if (h[5] & 0x40) {                  /* extended header */
        uint8_t e[4];
        if (fread(e, 1, 4, f) != 4) goto done;
        pos += v4 ? be32(e, true) : 4 + be32(e, false);
    }
    while (pos + 10 <= end) {
        uint8_t fh[10];
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(fh, 1, 10, f) != 10 || fh[0] == 0) break;
        uint32_t fs = be32(fh + 4, v4);
        if (fs == 0 || pos + 10 + fs > end) break;
        if (memcmp(fh, "APIC", 4) == 0) {
            uint8_t b[256];
            size_t n = fread(b, 1, fs < sizeof b ? fs : sizeof b, f);
            size_t k = 1;                               /* text encoding */
            while (k < n && b[k]) k++;                  /* MIME type */
            k += 2;                                     /* its zero, picture type */
            if (b[0] == 1 || b[0] == 2) {               /* UTF-16 description */
                while (k + 1 < n && (b[k] || b[k + 1])) k += 2;
                k += 2;
            } else {
                while (k < n && b[k]) k++;
                k++;
            }
            if (k < n && k < fs) {
                *off = pos + 10 + (uint32_t)k;
                *size = fs - (uint32_t)k;
                ok = true;
            }
            break;
        }
        pos += 10 + fs;
    }
done:
    fclose(f);
    return ok;
}

/* ---- the thread ------------------------------------------------------------ */

static void run_job(const job_t *j, img_result_t *r)
{
    char file[200] = "";
    if (j->cache[0]) {
        mkdir(j->cache, 0755);          /* there already, most of the time */
        if (cache_name(j, file, sizeof file) && cache_read(file, j, r)) return;
    }
    uint32_t off = j->offset, size = j->size;
    if (j->id3 && !find_apic(j->path, &off, &size)) return;
    r->px = aos_hal_image_decode(j->path, off, size, j->w, j->h, j->fill,
                                 &r->w, &r->h, &r->src_w, &r->src_h);
    r->kind_ = r->px ? KIND_HAL : KIND_NONE;
    if (r->px && file[0]) cache_write(file, r);
}

static int pick(void)
{
    int best = -1;
    for (int i = 0; i < s_nq; i++) {
        const job_t *q = &s_q[i];
        if (best < 0) { best = i; continue; }
        const job_t *b = &s_q[best];
        if (q->urgent != b->urgent) {
            if (q->urgent) best = i;
        } else if (q->urgent ? q->seq < b->seq : q->seq > b->seq) {
            best = i;                   /* urgent in order, the rest newest first */
        }
    }
    return best;
}

static void loader_main(void *arg)
{
    (void)arg;
    static job_t job;                   /* one thread at a time: off its stack */
    for (;;) {
        aos_hal_mutex_lock(s_mux);
        if (s_nq == 0 || s_nd >= DMAX) {
            /* nothing to do, or nobody collecting (the app is in the back):
             * gone, and img_take() starts it again */
            s_thread_on = false;
            aos_hal_mutex_unlock(s_mux);
            return;
        }
        int k = pick();
        job = s_q[k];
        s_q[k] = s_q[--s_nq];
        s_run_owner = job.owner;
        s_run_cancelled = false;
        aos_hal_mutex_unlock(s_mux);

        img_result_t r = { .tag = job.tag };
        run_job(&job, &r);

        aos_hal_mutex_lock(s_mux);
        bool keep = !s_run_cancelled && s_nd < DMAX;
        if (keep) {
            s_done[s_nd] = r;
            s_done_owner[s_nd] = job.owner;
            s_nd++;
        }
        s_run_owner = 0;
        aos_hal_mutex_unlock(s_mux);
        if (!keep) img_free(&r);
    }
}

/* With the lock held. */
static void kick(void)
{
    if (s_thread_on || s_nq == 0 || s_nd >= DMAX) return;
    s_thread_on = true;
    /* 8 KB: fopen/fread through FATFS and the decoder's own calls */
    if (!aos_hal_thread_start("aos_img", loader_main, NULL, 8192, 3)) s_thread_on = false;
}

/* ---- the app's side (LVGL's task) ------------------------------------------ */

bool img_request(int owner, uintptr_t tag, const img_job_t *job)
{
    if (!job || !job->path || job->w <= 0 || job->h <= 0) return false;
    if (!s_mux) s_mux = aos_hal_mutex_create();
    if (!s_mux) return false;
    aos_hal_mutex_lock(s_mux);
    bool ok = s_nq < QMAX;
    if (ok) {
        job_t *q = &s_q[s_nq++];
        memset(q, 0, sizeof *q);
        q->owner = owner;
        q->tag = tag;
        q->seq = ++s_seq;
        q->urgent = job->urgent;
        q->fill = job->fill;
        q->id3 = job->id3;
        q->offset = job->offset;
        q->size = job->size;
        q->w = job->w;
        q->h = job->h;
        snprintf(q->path, sizeof q->path, "%s", job->path);
        if (job->cache_dir) snprintf(q->cache, sizeof q->cache, "%s", job->cache_dir);
        kick();
    }
    aos_hal_mutex_unlock(s_mux);
    return ok;
}

bool img_take(int owner, img_result_t *out)
{
    if (!s_mux) return false;
    bool found = false;
    aos_hal_mutex_lock(s_mux);
    for (int i = 0; i < s_nd; i++) {
        if (s_done_owner[i] == owner) {
            *out = s_done[i];
            s_nd--;
            memmove(&s_done[i], &s_done[i + 1], (size_t)(s_nd - i) * sizeof s_done[0]);
            memmove(&s_done_owner[i], &s_done_owner[i + 1], (size_t)(s_nd - i) * sizeof(int));
            found = true;
            break;
        }
    }
    kick();                             /* room again: go on if it had stopped */
    aos_hal_mutex_unlock(s_mux);
    return found;
}

void img_cancel(int owner, uintptr_t tag)
{
    if (!s_mux) return;
    aos_hal_mutex_lock(s_mux);
    for (int i = 0; i < s_nq;) {
        if (s_q[i].owner == owner && s_q[i].tag == tag) s_q[i] = s_q[--s_nq];
        else i++;
    }
    aos_hal_mutex_unlock(s_mux);
}

void img_cancel_all(int owner)
{
    if (!s_mux) return;
    img_result_t drop[DMAX];
    int n = 0;
    aos_hal_mutex_lock(s_mux);
    for (int i = 0; i < s_nq;) {
        if (s_q[i].owner == owner) s_q[i] = s_q[--s_nq];
        else i++;
    }
    if (s_run_owner == owner) s_run_cancelled = true;
    for (int i = 0; i < s_nd;) {
        if (s_done_owner[i] == owner) {
            drop[n++] = s_done[i];
            s_nd--;
            memmove(&s_done[i], &s_done[i + 1], (size_t)(s_nd - i) * sizeof s_done[0]);
            memmove(&s_done_owner[i], &s_done_owner[i + 1], (size_t)(s_nd - i) * sizeof(int));
        } else {
            i++;
        }
    }
    kick();                             /* the other owner may be waiting */
    aos_hal_mutex_unlock(s_mux);
    for (int i = 0; i < n; i++) img_free(&drop[i]);
}
