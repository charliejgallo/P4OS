/*
 * P4OS - colour emoji. See aos_emoji.h.
 *
 * The pack (tools/gen_emoji.py) has an index of every sequence and one QOI
 * image per emoji, 48 px. The index lives in PSRAM (~200 KB); the images stay
 * on the card and are read when a text first needs one.
 *
 * In the text an emoji is one code point of plane 15's private-use area,
 * U+F0000 + its place in the index: aos_text_safe() writes it in place of the
 * whole sequence (skin tones, ZWJ families, flags, keycaps), so LVGL, which
 * shapes nothing, has a single glyph to look up.
 *
 * Each theme font gets a copy whose fallback is an emoji font with the same
 * metrics. Asked for one of those code points, it decodes the image, scales
 * it to the line (area average, premultiplied, so the edges do not darken)
 * and keeps the result as an ARGB8888 image in PSRAM: LVGL draws glyphs of
 * format IMAGE as images, in their own colours.
 *
 * Glyphs are asked for from the draw threads as well as from LVGL's task, so
 * the cache is behind a mutex, and nothing in it is ever freed: a draw thread
 * may be painting it. It stops growing at CACHE_MAX bytes, which is some
 * thousands of emoji at text size; past that, a new one is not drawn.
 */
#include "aos_emoji.h"
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define MAGIC       "AEMJ"
#define MAXCP       10
#define ENTRY       (4 + 4 * MAXCP + 8)
#define PUA_BASE    0xF0000u
#define CACHE_MAX   (6u * 1024 * 1024)
#define HASH_N      4096                /* a power of two */

typedef struct {
    uint8_t n;
    uint32_t cp[MAXCP];
    uint32_t off, len;
} entry_t;

static FILE *s_f;
static entry_t *s_idx;
static uint32_t s_count, s_data_off;
static int s_size;                      /* the images' side, px */
static void *s_mx;

typedef struct {
    uint32_t key;                       /* id << 8 | px, 0 = empty */
    lv_image_dsc_t *img;
} slot_t;
static slot_t *s_cache;
static size_t s_cache_bytes;

/* ---- the pack ---- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool aos_emoji_init(void)
{
    if (s_idx) return true;
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char path[96];
    snprintf(path, sizeof path, "%s/fonts/emoji.pak", root);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[20];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, MAGIC, 4) || (h[4] | h[5] << 8) != 1) {
        fclose(f);
        return false;
    }
    uint32_t count = rd32(h + 8), io = rd32(h + 12), dofs = rd32(h + 16);
    uint8_t *raw = lv_malloc((size_t)count * ENTRY);
    entry_t *idx = lv_malloc((size_t)count * sizeof *idx);
    slot_t *cache = lv_malloc(HASH_N * sizeof *cache);
    if (!raw || !idx || !cache || fseek(f, (long)io, SEEK_SET) || fread(raw, ENTRY, count, f) != count) {
        lv_free(raw);
        lv_free(idx);
        lv_free(cache);
        fclose(f);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = raw + (size_t)i * ENTRY;
        idx[i].n = e[0] > MAXCP ? MAXCP : e[0];
        for (int k = 0; k < MAXCP; k++) idx[i].cp[k] = rd32(e + 4 + 4 * k);
        idx[i].off = rd32(e + 4 + 4 * MAXCP);
        idx[i].len = rd32(e + 8 + 4 * MAXCP);
    }
    lv_free(raw);
    memset(cache, 0, HASH_N * sizeof *cache);
    s_mx = aos_hal_mutex_create();
    s_f = f;
    s_count = count;
    s_data_off = dofs;
    s_size = h[6] | h[7] << 8;
    s_cache = cache;
    s_idx = idx;
    aos_hal_log("emoji", "%s: %u emoji at %d px", path, (unsigned)count, s_size);
    return true;
}

bool aos_emoji_ready(void) { return s_idx != NULL; }

/* ---- matching ---- */

static int cmp_seq(const uint32_t *a, int an, const entry_t *e)
{
    int n = an < e->n ? an : e->n;
    for (int i = 0; i < n; i++)
        if (a[i] != e->cp[i]) return a[i] < e->cp[i] ? -1 : 1;
    return an - e->n;
}

static int find(const uint32_t *seq, int n)
{
    int lo = 0, hi = (int)s_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        int c = cmp_seq(seq, n, &s_idx[m]);
        if (!c) return m;
        if (c < 0) hi = m - 1;
        else lo = m + 1;
    }
    return -1;
}

static int utf8(const unsigned char *s, size_t len, uint32_t *cp)
{
    unsigned char c = s[0];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!n || (size_t)n > len) return 0;
    uint32_t v = n == 1 ? c : c & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}

/* Symbols of the Basic Multilingual Plane that are emoji with no variation
 * selector after them (Unicode's Emoji_Presentation=Yes there). The other
 * single BMP ones, like the heart or the sun, are text unless U+FE0F follows,
 * and a lone # or digit is never an emoji: only as a keycap. */
static bool bmp_emoji_by_default(uint32_t c)
{
    static const uint16_t R[][2] = {
        { 0x231A, 0x231B }, { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
        { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F },
        { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE },
        { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
        { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
        { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 }, { 0x274C, 0x274C },
        { 0x274E, 0x274E }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
        { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 },
        { 0x2B55, 0x2B55 },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++)
        if (c >= R[i][0] && c <= R[i][1]) return true;
    return false;
}

size_t aos_emoji_match(const char *str, size_t len, uint32_t *out)
{
    if (!s_idx || !len) return 0;
    const unsigned char *s = (const unsigned char *)str;
    uint32_t first;
    if (!utf8(s, len, &first)) return 0;
    /* only what can start an emoji: no letter or punctuation pays for a search */
    if (first < 0x2000 && first != '#' && first != '*' && !(first >= '0' && first <= '9') && first != 0xA9 &&
        first != 0xAE)
        return 0;

    uint32_t seq[MAXCP];
    size_t end[MAXCP];                  /* bytes up to and including seq[k] */
    bool vs[MAXCP];                     /* U+FE0F right after seq[k] */
    int n = 0;
    size_t i = 0;
    while (i < len && n < MAXCP) {
        uint32_t c;
        int k = utf8(s + i, len - i, &c);
        if (!k) break;
        if (c == 0xFE0F || c == 0xFE0E) {
            if (!n) break;
            vs[n - 1] = c == 0xFE0F;
            end[n - 1] = i + (size_t)k;
            i += (size_t)k;
            continue;
        }
        /* a sequence goes on only through joiners, modifiers, tags and keycap
         * frames, or a second regional indicator */
        if (n && !(c == 0x200D || seq[n - 1] == 0x200D || (c >= 0x1F3FB && c <= 0x1F3FF) ||
                   (c >= 0xE0020 && c <= 0xE007F) || c == 0x20E3 ||
                   (n == 1 && c >= 0x1F1E6 && c <= 0x1F1FF && seq[0] >= 0x1F1E6 && seq[0] <= 0x1F1FF)))
            break;
        seq[n] = c;
        vs[n] = false;
        end[n] = i + (size_t)k;
        n++;
        i += (size_t)k;
    }
    for (int l = n; l > 0; l--) {
        if (seq[l - 1] == 0x200D) continue;     /* never end on a joiner */
        int id = find(seq, l);
        if (id < 0) continue;
        if (l == 1 && seq[0] < 0x10000 && !vs[0] && !bmp_emoji_by_default(seq[0])) return 0;
        *out = PUA_BASE + (uint32_t)id;
        return end[l - 1];
    }
    return 0;
}

/* ---- images ---- */

/* QOI (qoiformat.org) into RGBA. */
static bool qoi_decode(const uint8_t *b, size_t len, uint8_t *px, int w, int h)
{
    if (len < 22 || memcmp(b, "qoif", 4)) return false;
    uint32_t bw = (uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7];
    uint32_t bh = (uint32_t)b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11];
    if ((int)bw != w || (int)bh != h) return false;
    uint8_t idx[64][4];
    memset(idx, 0, sizeof idx);
    uint8_t p[4] = { 0, 0, 0, 255 };
    size_t at = 14, n = (size_t)w * h, run = 0;
    for (size_t o = 0; o < n; o++) {
        if (run) {
            run--;
        } else if (at < len - 8) {
            uint8_t c = b[at++];
            if (c == 0xFE) {
                p[0] = b[at]; p[1] = b[at + 1]; p[2] = b[at + 2];
                at += 3;
            } else if (c == 0xFF) {
                memcpy(p, b + at, 4);
                at += 4;
            } else if ((c >> 6) == 0) {
                memcpy(p, idx[c], 4);
            } else if ((c >> 6) == 1) {
                p[0] += ((c >> 4) & 3) - 2;
                p[1] += ((c >> 2) & 3) - 2;
                p[2] += (c & 3) - 2;
            } else if ((c >> 6) == 2) {
                uint8_t c2 = b[at++];
                int dg = (c & 63) - 32;
                p[0] += dg - 8 + (c2 >> 4);
                p[1] += dg;
                p[2] += dg - 8 + (c2 & 15);
            } else {
                run = c & 63;
            }
            memcpy(idx[(p[0] * 3 + p[1] * 5 + p[2] * 7 + p[3] * 11) % 64], p, 4);
        } else {
            return false;
        }
        memcpy(px + o * 4, p, 4);
    }
    return true;
}

/* src (s x s RGBA) to an ARGB8888 image of d x d, by area average over
 * premultiplied colour (or nearest when growing, rare: only the 64 px font). */
static lv_image_dsc_t *scaled(const uint8_t *src, int s, int d)
{
    size_t bytes = sizeof(lv_image_dsc_t) + (size_t)d * d * 4;
    if (s_cache_bytes + bytes > CACHE_MAX) return NULL;
    lv_image_dsc_t *img = lv_malloc(bytes);
    if (!img) return NULL;
    memset(img, 0, sizeof *img);
    uint8_t *dst = (uint8_t *)(img + 1);
    for (int y = 0; y < d; y++) {
        int y0 = y * s / d, y1 = (y + 1) * s / d;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < d; x++) {
            int x0 = x * s / d, x1 = (x + 1) * s / d;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t *p = src + ((size_t)yy * s + xx) * 4;
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    n++;
                }
            uint8_t *q = dst + ((size_t)y * d + x) * 4;
            q[3] = (uint8_t)(a / n);
            q[2] = a ? (uint8_t)(r / a) : 0;     /* ARGB8888 is B, G, R, A in memory */
            q[1] = a ? (uint8_t)(g / a) : 0;
            q[0] = a ? (uint8_t)(b / a) : 0;
        }
    }
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_ARGB8888;
    img->header.w = (uint32_t)d;
    img->header.h = (uint32_t)d;
    img->header.stride = (uint32_t)d * 4;
    img->data_size = (uint32_t)d * d * 4;
    img->data = dst;
    s_cache_bytes += bytes;
    return img;
}

/* The image of emoji 'id' at d px, made once. Under the mutex. */
static lv_image_dsc_t *image(uint32_t id, int d)
{
    uint32_t key = id << 8 | (uint32_t)d;
    uint32_t h = (key * 2654435761u) & (HASH_N - 1);
    for (int probe = 0; probe < HASH_N; probe++, h = (h + 1) & (HASH_N - 1)) {
        if (s_cache[h].key == key) return s_cache[h].img;
        if (!s_cache[h].key) break;
    }
    if (s_cache[h].key) return NULL;    /* the table is full */
    const entry_t *e = &s_idx[id];
    uint8_t *q = lv_malloc(e->len), *px = lv_malloc((size_t)s_size * s_size * 4);
    lv_image_dsc_t *img = NULL;
    if (q && px && !fseek(s_f, (long)(s_data_off + e->off), SEEK_SET) && fread(q, 1, e->len, s_f) == e->len &&
        qoi_decode(q, e->len, px, s_size, s_size))
        img = scaled(px, s_size, d);
    lv_free(q);
    lv_free(px);
    if (img) {
        s_cache[h].key = key;
        s_cache[h].img = img;
    }
    return img;
}

/* ---- the fonts ---- */

typedef struct {
    lv_font_t font;                     /* first: LVGL hands this pointer back */
    int d;                              /* the emoji's side at this size */
    int adv;
    int ofs_y;
} emoji_font_t;

static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
    (void)next;
    if (cp < PUA_BASE || cp >= PUA_BASE + s_count) return false;
    const emoji_font_t *ef = (const emoji_font_t *)font;
    aos_hal_mutex_lock(s_mx);
    lv_image_dsc_t *img = image(cp - PUA_BASE, ef->d);
    aos_hal_mutex_unlock(s_mx);
    if (!img) return false;
    g->is_placeholder = 0;
    g->adv_w = (uint16_t)ef->adv;
    g->box_w = (uint16_t)ef->d;
    g->box_h = (uint16_t)ef->d;
    g->ofs_x = (int16_t)((ef->adv - ef->d) / 2);
    g->ofs_y = (int16_t)ef->ofs_y;
    g->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    g->gid.src = img;
    return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    (void)buf;
    return g->gid.src;
}

const lv_font_t *aos_emoji_font(const lv_font_t *font)
{
    if (!s_idx || !font) return font;
    lv_font_t *copy = lv_malloc(sizeof *copy);
    emoji_font_t *ef = lv_malloc(sizeof *ef);
    if (!copy || !ef) {
        lv_free(copy);
        lv_free(ef);
        return font;
    }
    *copy = *font;
    memset(ef, 0, sizeof *ef);
    int lh = font->line_height, bl = font->base_line;
    /* a little under the line, as a phone draws them next to text */
    ef->d = lh * 4 / 5;
    ef->adv = ef->d + ef->d / 8;
    /* centred on the line: LVGL puts a glyph's top at
     * line_height - base_line - box_h - ofs_y of the font being drawn */
    ef->ofs_y = lh - bl - ef->d - (lh - ef->d) / 2;
    ef->font.get_glyph_dsc = glyph_dsc;
    ef->font.get_glyph_bitmap = glyph_bitmap;
    ef->font.line_height = font->line_height;
    ef->font.base_line = font->base_line;
    ef->font.subpx = LV_FONT_SUBPX_NONE;
    ef->font.fallback = font->fallback;
    copy->fallback = &ef->font;
    return copy;
}
